#include "core/endtask.h"

#include "core/linuxparsers.h"

#ifndef __linux__
#error "Ending tasks uses Linux pidfds and only supports Linux."
#endif

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace ps::endtask {

namespace {

constexpr long kProcSuperMagic = 0x9fa0; // PROC_SUPER_MAGIC, <linux/magic.h>
constexpr std::size_t kStatCap = 4096;
constexpr std::size_t kStatusCap = 64 * 1024;
constexpr std::size_t kFdinfoCap = 4096;

class Fd {
public:
    explicit Fd(int fd = -1)
        : m_fd(fd)
    {
    }
    ~Fd()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }

private:
    int m_fd;
};

int sysPidfdOpen(int pid)
{
#if defined(SYS_pidfd_open)
    return static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0u));
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

int sysPidfdSendSignal(int pidfd, int sig)
{
#if defined(SYS_pidfd_send_signal)
    return static_cast<int>(::syscall(SYS_pidfd_send_signal, pidfd, sig, static_cast<void*>(nullptr), 0u));
#else
    (void)pidfd;
    (void)sig;
    errno = ENOSYS;
    return -1;
#endif
}

// Reads all of dirfd-relative `rel`. False with err set on failure, and
// EFBIG for a file longer than cap: a prefix is never taken for the whole.
bool readAll(int dirfd, const char* rel, std::string& out, std::size_t cap, int& err)
{
    out.clear();
    const Fd fd(::openat(dirfd, rel, O_RDONLY | O_CLOEXEC | O_NOCTTY));
    if (!fd.valid()) {
        err = errno;
        return false;
    }
    char buf[4096];
    for (;;) {
        const std::size_t want = std::min(sizeof buf, cap + 1 - out.size());
        const ssize_t n = ::read(fd.get(), buf, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            err = errno;
            return false;
        }
        if (n == 0)
            return true;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > cap) {
            err = EFBIG;
            return false;
        }
    }
}

// "/proc" as the kernel's process filesystem, or -1 with err (0: another
// filesystem is mounted there).
int openProc(int& err)
{
    const int fd = ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        err = errno;
        return -1;
    }
    struct statfs fs {};
    const bool checked = ::fstatfs(fd, &fs) == 0;
    if (!checked || static_cast<long>(fs.f_type) != kProcSuperMagic) {
        err = checked ? 0 : errno;
        ::close(fd);
        return -1;
    }
    return fd;
}

// nullopt if this /proc numbers processes as bex's own PID namespace
// does: /proc/self is our PID, and its NSpid line has that single entry (the
// procfs of an ancestor namespace lists one entry per level). Otherwise why
// not, with err set for Failed.
std::optional<Outcome> foreignProc(int procfd, int selfPid, int& err)
{
    err = 0;
    char buf[32];
    const ssize_t n = ::readlinkat(procfd, "self", buf, sizeof buf);
    if (n < 0) {
        err = errno;
        if (err != ENOENT)
            return Outcome::Failed;
        err = 0; // bex is not visible in this /proc's namespace
        return Outcome::Unsupported;
    }
    const auto self = static_cast<std::size_t>(n) < sizeof buf
        ? parse::toI64(std::string_view(buf, static_cast<std::size_t>(n)))
        : std::nullopt;
    if (!self)
        return Outcome::Failed;
    if (*self != selfPid)
        return Outcome::Unsupported;
    std::string status;
    if (!readAll(procfd, "self/status", status, kStatusCap, err))
        return Outcome::Failed;
    for (auto line : parse::splitLines(status)) {
        if (line.substr(0, 6) != "NSpid:")
            continue;
        const auto ids = parse::splitWhitespace(line.substr(6));
        return ids.size() == 1 && parse::toI64(ids.front()) == selfPid ? std::nullopt
                                                                        : std::optional(Outcome::Unsupported);
    }
    return Outcome::Unsupported; // no NSpid (before Linux 4.1): cannot tell
}

// The "Pid:" line of the pidfd's fdinfo: the process's PID in the namespace
// of this /proc, 0 if it is not visible there, -1 once it has been reaped.
// nullopt with err set if the fdinfo could not be read, or with err 0 if the
// kernel does not report the PID.
std::optional<int64_t> pidfdPid(int procfd, int pidfd, int& err)
{
    const std::string rel = "self/fdinfo/" + std::to_string(pidfd);
    std::string text;
    err = 0;
    if (!readAll(procfd, rel.c_str(), text, kFdinfoCap, err))
        return std::nullopt;
    for (auto line : parse::splitLines(text)) {
        if (line.substr(0, 4) == "Pid:")
            return parse::toI64(parse::trim(line.substr(4)));
    }
    return std::nullopt;
}

bool exitedState(char state)
{
    return state == 'Z' || state == 'X' || state == 'x';
}

std::string quoted(const std::string& name)
{
    return "\xe2\x80\x9c" + name + "\xe2\x80\x9d";
}

std::string who(const Target& t)
{
    return quoted(t.name) + " (PID " + std::to_string(t.pid) + ")";
}

std::string errorText(int err)
{
    return std::error_code(err, std::generic_category()).message();
}

// Only the two requests exist; any other value is refused, never mapped.
std::optional<int> signalNumber(Signal signal)
{
    switch (signal) {
    case Signal::Terminate: return SIGTERM;
    case Signal::Kill: return SIGKILL;
    }
    return std::nullopt;
}

} // namespace

Target targetOf(const ProcessInfo& p)
{
    Target t;
    t.pid = p.pid;
    t.startTicks = p.startTicks;
    t.name = p.name;
    t.uid = p.uid;
    t.uidKnown = p.uidKnown;
    t.kernelThread = p.kernelThread;
    return t;
}

Outcome precheck(const Target& t, uint32_t ownUid, int selfPid)
{
    if (t.pid <= 0)
        return Outcome::Invalid;
    if (t.pid == 1 || t.pid == selfPid || t.kernelThread)
        return Outcome::Protected;
    if (!t.uidKnown || t.uid != ownUid)
        return Outcome::NotOwned;
    return Outcome::Sent;
}

Result send(const Target& t, Signal signal, const Hooks& hooks)
{
    Result r;
    const auto done = [&](Outcome o, int err = 0) {
        r.outcome = o;
        r.error = err;
        return r;
    };
    const auto sig = signalNumber(signal);
    if (!sig)
        return done(Outcome::Invalid);
    const uint32_t ownUid = hooks.ownUid ? *hooks.ownUid : static_cast<uint32_t>(::getuid());
    const int selfPid = static_cast<int>(::getpid());
    if (const Outcome o = precheck(t, ownUid, selfPid); o != Outcome::Sent)
        return done(o);

    int err = 0;
    const Fd proc(openProc(err));
    if (!proc.valid())
        return done(err == 0 ? Outcome::Unsupported : Outcome::Failed, err);
    // Another PID namespace's /proc: its numbers are not the ones pidfd_open
    // resolves.
    if (const auto o = foreignProc(proc.get(), selfPid, err))
        return done(*o, err);

    // The descriptor comes first: whatever is checked below, the signal can
    // only reach the process it was opened for.
    errno = 0;
    const Fd pidfd(hooks.pidfdOpen ? hooks.pidfdOpen(t.pid) : sysPidfdOpen(t.pid));
    if (!pidfd.valid()) {
        err = errno;
        switch (err) {
        case ESRCH: return done(Outcome::Exited, err);
        case ENOSYS:
        case EPERM: return done(Outcome::Unsupported, err);
        case EINVAL: return done(Outcome::Changed, err); // not a process (thread-group leader) any more
        default: return done(Outcome::Failed, err);
        }
    }
    // It must be the process this /proc lists under that number.
    const auto bound = [&](int& e) -> std::optional<Outcome> {
        const auto nr = pidfdPid(proc.get(), pidfd.get(), e);
        if (e != 0)
            return Outcome::Failed; // fdinfo unreadable (say, out of descriptors): unchecked, not unsupported
        if (!nr)
            return Outcome::Unsupported; // no fdinfo Pid: cannot match the descriptor to /proc
        if (*nr == -1)
            return Outcome::Exited;
        if (*nr != t.pid)
            return Outcome::Unsupported; // 0: not visible here; other: another PID namespace
        return std::nullopt;
    };
    int boundErr = 0;
    if (const auto o = bound(boundErr))
        return done(*o, boundErr);

    // The live identity, read while the descriptor holds the PID.
    const std::string pid = std::to_string(t.pid);
    const Fd dir(::openat(proc.get(), pid.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!dir.valid())
        err = errno;
    std::string stat, status;
    const bool read = dir.valid() && readAll(dir.get(), "stat", stat, kStatCap, err)
        && readAll(dir.get(), "status", status, kStatusCap, err);
    // Still held after the reads: no other process can have had this PID in
    // between, so what was read belongs to the descriptor's process.
    if (const auto o = bound(boundErr))
        return done(*o, boundErr);
    if (!read)
        return done(err == ENOENT || err == ESRCH ? Outcome::Exited : Outcome::Failed, err);
    const auto st = parse::parsePidStat(stat);
    const auto uid = parse::parseStatusRealUid(status);
    if (!st || !uid)
        return done(Outcome::Failed);
    r.state = st->state;
    r.kernelThread = (st->flags & parse::kPfKthread) != 0;
    if (st->pid != t.pid || st->startTime != t.startTicks || st->comm != t.name)
        return done(Outcome::Changed);
    if (r.kernelThread)
        return done(Outcome::Protected);
    if (exitedState(st->state))
        return done(Outcome::Exited);
    if (*uid != t.uid)
        return done(Outcome::Changed);
    if (*uid != ownUid)
        return done(Outcome::NotOwned);

    if (hooks.beforeSend)
        hooks.beforeSend();
    errno = 0;
    if ((hooks.sendSignal ? hooks.sendSignal(pidfd.get(), *sig) : sysPidfdSendSignal(pidfd.get(), *sig)) == 0)
        return done(Outcome::Sent);
    err = errno;
    switch (err) {
    case ESRCH: return done(Outcome::Exited, err);
    case EPERM: return done(Outcome::Denied, err);
    case ENOSYS: return done(Outcome::Unsupported, err);
    default: return done(Outcome::Failed, err);
    }
}

std::string describe(const Result& r, const Target& t, Signal signal)
{
    const std::string none = " Nothing was sent.";
    if (!signalNumber(signal))
        return "That is neither End task nor Force end task." + none;
    const bool force = signal == Signal::Kill;
    switch (r.outcome) {
    case Outcome::Sent: {
        std::string s = (force ? "Force end (SIGKILL) sent to " : "End request (SIGTERM) sent to ") + who(t) + ".";
        if (!force && (r.state == 'T' || r.state == 't'))
            s += " It is stopped, so it cannot quit until it is continued.";
        return s;
    }
    case Outcome::Exited:
        if (r.state == 'Z')
            return who(t) + " has already exited; its parent has not collected it yet." + none;
        return who(t) + " has already exited." + none;
    case Outcome::Changed:
        return "PID " + std::to_string(t.pid) + " is no longer " + quoted(t.name)
            + ": the process was replaced or changed after it was listed." + none;
    case Outcome::NotOwned:
        if (!t.uidKnown)
            return "The owner of " + who(t) + " is unknown, and bex only ends your own processes." + none;
        return who(t) + " belongs to another user, and bex only ends your own processes." + none;
    case Outcome::Protected:
        if (t.pid == 1)
            return "PID 1 is the system's init process and is not ended from here." + none;
        if (t.kernelThread || r.kernelThread)
            return who(t) + " is a kernel thread and cannot be ended." + none;
        return who(t) + " is bex itself; quit it instead." + none;
    case Outcome::Invalid:
        return "The selected row has no usable process identity." + none;
    case Outcome::Denied:
        return "The system refused to signal " + who(t) + ": " + errorText(r.error) + "." + none;
    case Outcome::Unsupported:
        if (r.error == ENOSYS)
            return "Ending tasks needs process file descriptors (pidfd, Linux 5.3 or later)." + none;
        if (r.error != 0)
            return "The system does not allow process file descriptors (pidfd) here: " + errorText(r.error) + "."
                + none;
        return "/proc does not match bex's PID namespace, so " + who(t) + " cannot be matched safely."
            + none;
    case Outcome::Failed:
        break;
    }
    return "Could not check " + who(t) + ": "
        + (r.error != 0 ? errorText(r.error) : std::string("/proc gave an unexpected answer")) + "." + none;
}

} // namespace ps::endtask
