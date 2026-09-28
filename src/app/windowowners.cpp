#include "app/windowowners.h"

#include "core/linuxparsers.h"
#include "core/sampler.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef BEX_HAVE_X11
#include <xcb/res.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h> // xcb_poll_for_reply
#endif

namespace windows {

namespace {

constexpr int kPidMax = 4194304; // PID_MAX_LIMIT on 64-bit kernels

using SteadyClock = std::chrono::steady_clock;

int remainingMs(SteadyClock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - SteadyClock::now()).count();
    return left <= 0 ? 0 : static_cast<int>(std::min<long long>(left, 60000));
}

// Waits for `events` on fd, or for cancelFd, until the deadline.
enum class Wait { Ready, Timeout, Cancelled, Error };
Wait waitFor(int fd, short events, int cancelFd, SteadyClock::time_point deadline)
{
    for (;;) {
        pollfd fds[2] = {{fd, events, 0}, {cancelFd, POLLIN, 0}};
        const nfds_t n = cancelFd >= 0 ? 2 : 1;
        const int left = remainingMs(deadline);
        const int r = ::poll(fds, n, left);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return Wait::Error;
        }
        if (n == 2 && (fds[1].revents & POLLIN))
            return Wait::Cancelled;
        if (r == 0)
            return Wait::Timeout;
        if (fds[0].revents & (events | POLLHUP | POLLERR))
            return Wait::Ready;
        if (left == 0)
            return Wait::Timeout;
    }
}

struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    ~Fd()
    {
        if (fd >= 0)
            ::close(fd);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

// Reads at most cap bytes of dirfd-relative `rel`; -1 on failure.
ssize_t readSmall(int dirfd, const char* rel, char* buf, std::size_t cap)
{
    const Fd f(::openat(dirfd, rel, O_RDONLY | O_CLOEXEC | O_NOCTTY));
    if (f.fd < 0)
        return -1;
    std::size_t total = 0;
    while (total < cap) {
        const ssize_t n = ::read(f.fd, buf + total, cap - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        total += static_cast<std::size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

Query fail(Status status, QString detail)
{
    Query q;
    q.status = status;
    q.detail = std::move(detail);
    return q;
}

// Whether `pid` (as seen through procRoot) lives in procRoot's own PID
// namespace: its NSpid line then has exactly one entry. Kernels without
// NSpid (before 4.1) cannot tell; the process is then taken as local.
bool sameNamespace(const std::string& procRoot, int pid)
{
    const Fd dir(::open(procRoot.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (dir.fd < 0)
        return false;
    char buf[4097];
    const std::string rel = std::to_string(pid) + "/status";
    const ssize_t n = readSmall(dir.fd, rel.c_str(), buf, sizeof buf - 1);
    if (n <= 0)
        return false;
    const std::string_view text(buf, static_cast<std::size_t>(n));
    const auto at = text.find("\nNSpid:");
    if (at == std::string_view::npos)
        return true;
    auto line = text.substr(at + 7);
    line = line.substr(0, line.find('\n'));
    int fields = 0;
    bool inField = false;
    for (const char c : line) {
        const bool space = c == ' ' || c == '\t';
        if (!space && !inField)
            ++fields;
        inField = !space;
    }
    return fields == 1;
}

} // namespace

// ---------------------------------------------------------------------------

const char* statusId(Status status)
{
    switch (status) {
    case Status::Ok: return "ok";
    case Status::Unsupported: return "unsupported";
    case Status::MissingSession: return "missing-session";
    case Status::NotRunning: return "not-running";
    case Status::PermissionDenied: return "permission-denied";
    case Status::OtherUser: return "other-user";
    case Status::OtherNamespace: return "other-namespace";
    case Status::Timeout: return "timeout";
    case Status::Disconnected: return "disconnected";
    case Status::Truncated: return "truncated";
    case Status::TooLarge: return "too-large";
    case Status::TooMany: return "too-many";
    case Status::Malformed: return "malformed";
    case Status::NotArray: return "not-array";
    case Status::Cancelled: return "cancelled";
    case Status::Failed: return "failed";
    }
    return "failed";
}

bool isFinal(Status status)
{
    switch (status) {
    case Status::Unsupported:
    case Status::MissingSession:
    case Status::OtherUser:
    case Status::OtherNamespace: return true;
    default: return false;
    }
}

QString backendName(Backend backend)
{
    switch (backend) {
    case Backend::Hyprland: return QStringLiteral("Hyprland");
    case Backend::X11: return QStringLiteral("X11");
    case Backend::None: break;
    }
    return QStringLiteral("None");
}

Environment Environment::fromProcess()
{
    auto get = [](const char* name) {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    };
    Environment e;
    e.hyprlandSignature = get("HYPRLAND_INSTANCE_SIGNATURE");
    e.runtimeDir = get("XDG_RUNTIME_DIR");
    e.waylandDisplay = get("WAYLAND_DISPLAY");
    e.sessionType = get("XDG_SESSION_TYPE");
    e.display = get("DISPLAY");
    return e;
}

bool x11Built()
{
#ifdef BEX_HAVE_X11
    return true;
#else
    return false;
#endif
}

Selection selectBackend(const Environment& env, bool x11Available)
{
    Selection s;
    if (!env.hyprlandSignature.empty()) {
        s.backend = Backend::Hyprland;
        const std::string& sig = env.hyprlandSignature;
        const bool badSig = sig == "." || sig == ".." || sig.find('/') != std::string::npos
            || sig.find('\0') != std::string::npos || sig.size() > 200;
        if (badSig) {
            s.status = Status::MissingSession;
            s.detail = QStringLiteral("HYPRLAND_INSTANCE_SIGNATURE is not a valid instance name");
            return s;
        }
        if (env.runtimeDir.empty() || env.runtimeDir.front() != '/') {
            s.status = Status::MissingSession;
            s.detail = QStringLiteral("XDG_RUNTIME_DIR is not set, so Hyprland's socket cannot be found");
            return s;
        }
        s.address = env.runtimeDir + "/hypr/" + sig + "/.socket.sock";
        if (s.address.size() >= sizeof(sockaddr_un::sun_path)) {
            s.status = Status::MissingSession;
            s.detail = QStringLiteral("Hyprland's socket path is too long");
            return s;
        }
        s.status = Status::Ok;
        return s;
    }
    if (!env.waylandDisplay.empty() || env.sessionType == "wayland") {
        s.detail = QStringLiteral("This Wayland desktop does not share its window list; bex can read it "
                                  "from Hyprland or an X11 session");
        return s;
    }
    if (!env.display.empty()) {
        s.backend = Backend::X11;
        if (!x11Available) {
            s.detail = QStringLiteral("This build of bex has no X11 support");
            return s;
        }
        s.address = env.display;
        s.status = Status::Ok;
        return s;
    }
    s.detail = QStringLiteral("No graphical session found (WAYLAND_DISPLAY and DISPLAY are not set)");
    return s;
}

// ---------------------------------------------------------------------------
// Hyprland

Query parseHyprlandClients(const QByteArray& reply, const Limits& limits)
{
    qsizetype end = reply.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(reply[end - 1])))
        --end;
    qsizetype begin = 0;
    while (begin < end && std::isspace(static_cast<unsigned char>(reply[begin])))
        ++begin;
    if (begin == end)
        return fail(Status::Disconnected, QStringLiteral("Hyprland closed the connection without replying"));

    QJsonParseError error {};
    const QJsonDocument doc = QJsonDocument::fromJson(reply, &error);
    if (error.error != QJsonParseError::NoError) {
        const char first = reply[begin];
        if (first != '[' && first != '{')
            return fail(Status::NotArray, QStringLiteral("Hyprland's reply is not a window list"));
        // Valid up to where the data stops: the reply was cut short.
        if (error.offset >= end)
            return fail(Status::Truncated, QStringLiteral("Hyprland's reply ended early"));
        return fail(Status::Malformed, QStringLiteral("Hyprland's reply is not valid JSON"));
    }
    if (!doc.isArray())
        return fail(Status::NotArray, QStringLiteral("Hyprland's reply is not a window list"));
    const QJsonArray list = doc.array();
    if (list.size() > limits.maxWindows)
        return fail(Status::TooMany, QStringLiteral("Hyprland reported more than %1 windows").arg(limits.maxWindows));

    Query q;
    q.status = Status::Ok;
    q.pids.reserve(static_cast<std::size_t>(list.size()));
    for (const QJsonValue& v : list) {
        if (!v.isObject())
            return fail(Status::Malformed, QStringLiteral("Hyprland's window list has an entry that is not a window"));
        ++q.windows;
        const QJsonValue pid = v.toObject().value(QLatin1String("pid"));
        const double d = pid.isDouble() ? pid.toDouble() : std::nan("");
        if (!std::isfinite(d) || d != std::floor(d) || d < 1 || d > kPidMax) {
            ++q.withoutPid;
            continue;
        }
        q.pids.push_back(static_cast<int>(d));
    }
    std::sort(q.pids.begin(), q.pids.end());
    q.pids.erase(std::unique(q.pids.begin(), q.pids.end()), q.pids.end());
    return q;
}

Query queryHyprland(const std::string& socketPath, const Limits& limits, int cancelFd, const std::string& procRoot)
{
    const auto deadline = SteadyClock::now() + std::chrono::milliseconds(std::max(1, limits.deadlineMs));
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (socketPath.empty() || socketPath.size() >= sizeof addr.sun_path)
        return fail(Status::MissingSession, QStringLiteral("Hyprland's socket path is not usable"));
    std::memcpy(addr.sun_path, socketPath.data(), socketPath.size());

    const Fd sock(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (sock.fd < 0)
        return fail(Status::Failed, QStringLiteral("Could not create a socket (%1)").arg(QString::fromLocal8Bit(std::strerror(errno))));

    const auto connectError = [](int err) {
        switch (err) {
        case ENOENT:
        case ENOTDIR: return fail(Status::NotRunning, QStringLiteral("Hyprland's IPC socket was not found"));
        case ECONNREFUSED:
            return fail(Status::NotRunning, QStringLiteral("Nothing is listening on Hyprland's IPC socket"));
        case EACCES:
        case EPERM: return fail(Status::PermissionDenied, QStringLiteral("Permission denied on Hyprland's IPC socket"));
        default:
            return fail(Status::Failed,
                        QStringLiteral("Could not connect to Hyprland (%1)").arg(QString::fromLocal8Bit(std::strerror(err))));
        }
    };
    const QString busy = QStringLiteral("Hyprland is busy: it did not accept the connection within %1 ms").arg(limits.deadlineMs);
    for (;;) {
        if (::connect(sock.fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0)
            break;
        const int err = errno;
        if (err == EISCONN)
            break;
        if (err == EINTR)
            continue;
        if (err == EAGAIN) {
            // A full listen backlog. Unlike EINPROGRESS no connection is
            // pending, so connect again shortly, until the deadline.
            const auto retryAt = std::min(deadline, SteadyClock::now() + std::chrono::milliseconds(10));
            if (waitFor(-1, 0, cancelFd, retryAt) == Wait::Cancelled)
                return fail(Status::Cancelled, QStringLiteral("Cancelled"));
            if (SteadyClock::now() >= deadline)
                return fail(Status::Timeout, busy);
            continue;
        }
        if (err != EINPROGRESS && err != EALREADY)
            return connectError(err);
        switch (waitFor(sock.fd, POLLOUT, cancelFd, deadline)) {
        case Wait::Ready: break;
        case Wait::Cancelled: return fail(Status::Cancelled, QStringLiteral("Cancelled"));
        case Wait::Timeout: return fail(Status::Timeout, busy);
        case Wait::Error: return fail(Status::Failed, QStringLiteral("Waiting for Hyprland failed"));
        }
        int soError = 0;
        socklen_t len = sizeof soError;
        if (::getsockopt(sock.fd, SOL_SOCKET, SO_ERROR, &soError, &len) != 0)
            soError = errno;
        if (soError == EAGAIN || soError == EINPROGRESS || soError == EALREADY)
            return fail(Status::Timeout, busy);
        if (soError != 0)
            return connectError(soError);
        break;
    }
    // Credentials are only meaningful once a peer is really connected; an
    // unconnected socket reports uid -1 and would read as another user.
    {
        sockaddr_un peer {};
        socklen_t peerLen = sizeof peer;
        if (::getpeername(sock.fd, reinterpret_cast<sockaddr*>(&peer), &peerLen) != 0)
            return errno == ENOTCONN ? fail(Status::Timeout, busy)
                                     : fail(Status::Failed, QStringLiteral("Could not confirm the connection to Hyprland (%1)")
                                                                .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }

    // Only this user's compositor, in this PID namespace, is believed.
    ucred cred {};
    socklen_t credLen = sizeof cred;
    if (::getsockopt(sock.fd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0)
        return fail(Status::Failed, QStringLiteral("Could not identify the process serving Hyprland's socket"));
    if (cred.uid != ::geteuid())
        return fail(Status::OtherUser, QStringLiteral("Hyprland's socket is served by another user"));
    if (cred.pid <= 0 || !sameNamespace(procRoot, cred.pid))
        return fail(Status::OtherNamespace,
                    QStringLiteral("Hyprland runs in another process namespace (for example, bex is sandboxed), "
                                   "so its process IDs cannot be matched"));

    static constexpr char kRequest[] = "j/clients";
    std::size_t sent = 0;
    while (sent < sizeof kRequest - 1) {
        const ssize_t n = ::send(sock.fd, kRequest + sent, sizeof kRequest - 1 - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) {
            const Wait w = waitFor(sock.fd, POLLOUT, cancelFd, deadline);
            if (w == Wait::Cancelled)
                return fail(Status::Cancelled, QStringLiteral("Cancelled"));
            if (w == Wait::Timeout)
                return fail(Status::Timeout, QStringLiteral("Hyprland did not take the request within %1 ms").arg(limits.deadlineMs));
            if (w == Wait::Error)
                return fail(Status::Failed, QStringLiteral("Waiting for Hyprland failed"));
            continue;
        }
        return fail(Status::Disconnected, QStringLiteral("Hyprland closed the connection before the request was sent"));
    }

    QByteArray reply;
    char buf[16384];
    for (;;) {
        const ssize_t n = ::recv(sock.fd, buf, sizeof buf, 0);
        if (n > 0) {
            if (reply.size() + n > limits.maxBytes)
                return fail(Status::TooLarge,
                            QStringLiteral("Hyprland's reply is larger than %1 KiB").arg(limits.maxBytes / 1024));
            reply.append(buf, n);
            continue;
        }
        if (n == 0)
            break; // Hyprland closes the connection after the reply
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN) {
            switch (waitFor(sock.fd, POLLIN, cancelFd, deadline)) {
            case Wait::Ready: continue;
            case Wait::Cancelled: return fail(Status::Cancelled, QStringLiteral("Cancelled"));
            case Wait::Timeout:
                return fail(Status::Timeout, reply.isEmpty()
                                ? QStringLiteral("Hyprland did not answer within %1 ms").arg(limits.deadlineMs)
                                : QStringLiteral("Hyprland's reply was incomplete after %1 ms").arg(limits.deadlineMs));
            case Wait::Error: return fail(Status::Failed, QStringLiteral("Waiting for Hyprland failed"));
            }
        }
        if (errno == ECONNRESET)
            return reply.isEmpty() ? fail(Status::Disconnected, QStringLiteral("Hyprland reset the connection"))
                                   : fail(Status::Truncated, QStringLiteral("Hyprland's reply ended early"));
        return fail(Status::Failed,
                    QStringLiteral("Reading Hyprland's reply failed (%1)").arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    return parseHyprlandClients(reply, limits);
}

// ---------------------------------------------------------------------------
// X11: _NET_CLIENT_LIST from the window manager, and each window's client
// PID from the X server itself (XRes 1.2, local clients only). _NET_WM_PID
// is not used: clients set it themselves and it may name a remote host.

#ifdef BEX_HAVE_X11
class X11Client {
public:
    ~X11Client() { disconnect(); }

    Query query(const std::string& display, const Limits& limits, int cancelFd, const std::string& procRoot)
    {
        const auto deadline = SteadyClock::now() + std::chrono::milliseconds(std::max(1, limits.deadlineMs));
        if (!m_conn) {
            if (Query q = connect(display, cancelFd, deadline, procRoot); q.status != Status::Ok) {
                disconnect();
                return q;
            }
        }
        Query q = list(limits, cancelFd, deadline);
        // A late server keeps its connection: reconnecting would block in
        // xcb_connect() while it is still stalled. Late replies are discarded.
        if (q.status != Status::Ok && q.status != Status::Timeout)
            disconnect(); // start from a clean connection next time
        return q;
    }

private:
    template <typename Reply>
    Status await(unsigned int sequence, Reply** reply, int cancelFd, SteadyClock::time_point deadline)
    {
        *reply = nullptr;
        for (;;) {
            void* r = nullptr;
            xcb_generic_error_t* e = nullptr;
            if (xcb_poll_for_reply(m_conn, sequence, &r, &e)) {
                std::free(e);
                if (!r)
                    return xcb_connection_has_error(m_conn) ? Status::Disconnected : Status::Failed;
                *reply = static_cast<Reply*>(r);
                return Status::Ok;
            }
            if (xcb_connection_has_error(m_conn))
                return Status::Disconnected;
            switch (waitFor(xcb_get_file_descriptor(m_conn), POLLIN, cancelFd, deadline)) {
            case Wait::Ready: break;
            case Wait::Cancelled: return Status::Cancelled;
            case Wait::Timeout: return Status::Timeout;
            case Wait::Error: return Status::Failed;
            }
        }
    }

    static Query statusQuery(Status s, const QString& what)
    {
        switch (s) {
        case Status::Timeout: return fail(s, QStringLiteral("The X server did not answer within the deadline (%1)").arg(what));
        case Status::Cancelled: return fail(s, QStringLiteral("Cancelled"));
        case Status::Disconnected: return fail(s, QStringLiteral("The X server closed the connection"));
        default: return fail(Status::Failed, QStringLiteral("The X server refused the request (%1)").arg(what));
        }
    }

    Query connect(const std::string& display, int cancelFd, SteadyClock::time_point deadline,
                  const std::string& procRoot)
    {
        int screen = 0;
        // xcb_connect() itself may block on an unresponsive server; the probe
        // thread is then abandoned on shutdown like a stuck collector. Every
        // later request is awaited within the deadline.
        m_conn = xcb_connect(display.c_str(), &screen);
        if (!m_conn || xcb_connection_has_error(m_conn))
            return fail(Status::NotRunning, QStringLiteral("Could not connect to the X server"));
        // XRes reports client PIDs as the X server sees them: they only name
        // local processes when the server is reached over a local socket and
        // shares this PID namespace (not over TCP or SSH forwarding).
        ucred cred {};
        socklen_t credLen = sizeof cred;
        if (::getsockopt(xcb_get_file_descriptor(m_conn), SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0
            || cred.pid <= 0)
            return fail(Status::Unsupported, QStringLiteral("The X server is not local, so its windows cannot be "
                                                            "matched to local processes"));
        if (!sameNamespace(procRoot, cred.pid))
            return fail(Status::OtherNamespace, QStringLiteral("The X server runs in another process namespace, so "
                                                               "its process IDs cannot be matched"));
        const xcb_setup_t* setup = xcb_get_setup(m_conn);
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
        for (int i = 0; i < screen && it.rem; ++i)
            xcb_screen_next(&it);
        if (!it.rem)
            return fail(Status::Failed, QStringLiteral("The X display has no such screen"));
        m_root = it.data->root;

        static constexpr char kClientList[] = "_NET_CLIENT_LIST";
        static constexpr char kXRes[] = "X-Resource";
        const auto atomCookie = xcb_intern_atom(m_conn, 1, sizeof kClientList - 1, kClientList);
        // Any XRes request first needs the extension's opcode, which libxcb
        // would otherwise fetch with a blocking round trip of its own (and a
        // missing extension would shut the connection). Prefetch it, then
        // wait for a later QueryExtension of ours within the deadline: replies
        // come in order, so the prefetched one has arrived by then.
        xcb_prefetch_extension_data(m_conn, &xcb_res_id);
        const auto extCookie = xcb_query_extension(m_conn, sizeof kXRes - 1, kXRes);
        xcb_flush(m_conn);
        xcb_intern_atom_reply_t* atom = nullptr;
        if (Status s = await(atomCookie.sequence, &atom, cancelFd, deadline); s != Status::Ok) {
            xcb_discard_reply(m_conn, extCookie.sequence);
            return statusQuery(s, QStringLiteral("atoms"));
        }
        m_clientList = atom->atom;
        std::free(atom);
        xcb_query_extension_reply_t* ext = nullptr;
        if (Status s = await(extCookie.sequence, &ext, cancelFd, deadline); s != Status::Ok)
            return statusQuery(s, QStringLiteral("extensions"));
        const bool hasRes = ext->present;
        std::free(ext);
        if (!hasRes)
            return fail(Status::Unsupported, QStringLiteral("The X server lacks the XRes 1.2 extension needed to "
                                                            "identify window owners"));
        if (!xcb_get_extension_data(m_conn, &xcb_res_id)) // no longer waits: the reply is in
            return statusQuery(Status::Disconnected, QStringLiteral("extensions"));
        const auto resCookie = xcb_res_query_version(m_conn, 1, 2);
        xcb_flush(m_conn);
        xcb_res_query_version_reply_t* res = nullptr;
        const Status rs = await(resCookie.sequence, &res, cancelFd, deadline);
        if (rs == Status::Timeout || rs == Status::Cancelled || rs == Status::Disconnected)
            return statusQuery(rs, QStringLiteral("XRes"));
        const bool resOk = res && (res->server_major > 1 || (res->server_major == 1 && res->server_minor >= 2));
        std::free(res);
        if (!resOk)
            return fail(Status::Unsupported, QStringLiteral("The X server lacks the XRes 1.2 extension needed to "
                                                            "identify window owners"));
        // No window manager (yet): worth asking again later.
        if (m_clientList == XCB_ATOM_NONE)
            return fail(Status::NotRunning, QStringLiteral("No window manager publishes _NET_CLIENT_LIST"));
        return fail(Status::Ok, {});
    }

    Query list(const Limits& limits, int cancelFd, SteadyClock::time_point deadline)
    {
        const auto propCookie = xcb_get_property(m_conn, 0, m_root, m_clientList, XCB_ATOM_WINDOW, 0,
                                                 static_cast<uint32_t>(limits.maxWindows));
        xcb_flush(m_conn);
        xcb_get_property_reply_t* prop = nullptr;
        if (Status s = await(propCookie.sequence, &prop, cancelFd, deadline); s != Status::Ok) {
            if (s == Status::Timeout)
                xcb_discard_reply(m_conn, propCookie.sequence);
            return statusQuery(s, QStringLiteral("window list"));
        }
        std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> propHolder(prop, &std::free);
        if (prop->type == XCB_ATOM_NONE)
            return fail(Status::NotRunning, QStringLiteral("No window manager publishes _NET_CLIENT_LIST"));
        if (prop->type != XCB_ATOM_WINDOW || prop->format != 32)
            return fail(Status::Malformed, QStringLiteral("_NET_CLIENT_LIST is not a list of windows"));
        if (prop->bytes_after > 0)
            return fail(Status::TooMany, QStringLiteral("The window manager lists more than %1 windows").arg(limits.maxWindows));
        const int count = xcb_get_property_value_length(prop) / 4;
        const auto* ids = static_cast<const xcb_window_t*>(xcb_get_property_value(prop));

        Query q;
        q.status = Status::Ok;
        q.windows = count;
        std::vector<unsigned int> cookies;
        cookies.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            xcb_res_client_id_spec_t spec {ids[i], XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
            cookies.push_back(xcb_res_query_client_ids(m_conn, 1, &spec).sequence);
        }
        xcb_flush(m_conn);
        for (std::size_t i = 0; i < cookies.size(); ++i) {
            xcb_res_query_client_ids_reply_t* reply = nullptr;
            const Status s = await(cookies[i], &reply, cancelFd, deadline);
            if (s == Status::Failed) { // e.g. BadWindow: the window just closed
                ++q.withoutPid;
                continue;
            }
            if (s != Status::Ok) {
                for (std::size_t j = s == Status::Timeout ? i : i + 1; j < cookies.size(); ++j)
                    xcb_discard_reply(m_conn, cookies[j]);
                return statusQuery(s, QStringLiteral("window owners"));
            }
            int pid = 0;
            for (auto it = xcb_res_query_client_ids_ids_iterator(reply); it.rem; xcb_res_client_id_value_next(&it)) {
                if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) && it.data->length >= 4) {
                    const uint32_t v = *xcb_res_client_id_value_value(it.data);
                    if (v >= 1 && v <= static_cast<uint32_t>(kPidMax))
                        pid = static_cast<int>(v);
                }
            }
            std::free(reply);
            if (pid > 0)
                q.pids.push_back(pid);
            else
                ++q.withoutPid; // a remote client, or the server cannot tell
        }
        std::sort(q.pids.begin(), q.pids.end());
        q.pids.erase(std::unique(q.pids.begin(), q.pids.end()), q.pids.end());
        return q;
    }

    void disconnect()
    {
        if (m_conn)
            xcb_disconnect(m_conn);
        m_conn = nullptr;
    }

    xcb_connection_t* m_conn = nullptr;
    xcb_window_t m_root = 0;
    xcb_atom_t m_clientList = XCB_ATOM_NONE;
};
#else
class X11Client {};
#endif

// ---------------------------------------------------------------------------

QString describe(const Result& r)
{
    if (r.complete())
        return {};
    if (r.ok()) {
        QStringList missing;
        if (r.withoutPid > 0)
            missing << (r.withoutPid == 1 ? QStringLiteral("1 window has no usable process ID")
                                          : QStringLiteral("%1 windows have no usable process ID").arg(r.withoutPid));
        if (r.unresolved > 0)
            missing << (r.unresolved == 1
                            ? QStringLiteral("1 window owner could not be read or had just exited")
                            : QStringLiteral("%1 window owners could not be read or had just exited").arg(r.unresolved));
        if (r.ambiguous > 0)
            missing << (r.ambiguous == 1
                            ? QStringLiteral("1 window owner started or was replaced while the list was read")
                            : QStringLiteral("%1 window owners started or were replaced while the list was read")
                                  .arg(r.ambiguous));
        return QStringLiteral("The window list from %1 is incomplete (%2)")
            .arg(backendName(r.backend), missing.join(QStringLiteral("; ")));
    }
    return r.detail.isEmpty() ? QStringLiteral("Window list unavailable (%1)").arg(QLatin1String(statusId(r.status)))
                              : r.detail;
}

void resolveOwners(const std::vector<int>& pids, double requested, const std::string& procRoot, long clockTicks,
                   Result& out)
{
    out.owners.clear();
    const Fd dir(::open(procRoot.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (dir.fd < 0) {
        out.status = Status::Failed;
        out.detail = QStringLiteral("%1 could not be read, so window owners cannot be identified")
                         .arg(QString::fromStdString(procRoot));
        return;
    }
    const long ticks = clockTicks > 0 ? clockTicks : std::max(1L, ::sysconf(_SC_CLK_TCK));
    char buf[4097];
    for (const int pid : pids) {
        char name[16];
        std::snprintf(name, sizeof name, "%d", pid);
        // One descriptor per process: if the PID is reused while it is read,
        // the read fails rather than describing the newcomer.
        const Fd pidfd(::openat(dir.fd, name, O_PATH | O_DIRECTORY | O_CLOEXEC));
        if (pidfd.fd < 0) {
            ++out.unresolved; // exited
            continue;
        }
        const ssize_t n = readSmall(pidfd.fd, "stat", buf, sizeof buf - 1);
        const auto st = n > 0 ? ps::parse::parsePidStat(std::string_view(buf, static_cast<std::size_t>(n)))
                              : std::nullopt;
        if (!st || st->pid != pid || (st->flags & ps::parse::kPfKthread)) {
            ++out.unresolved;
            continue;
        }
        // Started (to the clock tick) after the list was requested: the PID
        // now belongs to a newer process than the window's owner.
        const double started = static_cast<double>(st->startTime) / static_cast<double>(ticks);
        if (std::isfinite(requested) && started + 1.0 / static_cast<double>(ticks) > requested) {
            ++out.ambiguous;
            continue;
        }
        out.owners.push_back({pid, st->startTime});
    }
    std::sort(out.owners.begin(), out.owners.end());
    out.owners.erase(std::unique(out.owners.begin(), out.owners.end()), out.owners.end());
}

// ---------------------------------------------------------------------------

ProbeLink::ProbeLink()
    : cancelFd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
{
}

ProbeLink::~ProbeLink()
{
    if (cancelFd >= 0)
        ::close(cancelFd);
}

void ProbeLink::cancel()
{
    if (cancelFd >= 0) {
        const uint64_t one = 1;
        [[maybe_unused]] const ssize_t n = ::write(cancelFd, &one, sizeof one);
    }
}

ProbeWorker::ProbeWorker(ProbeOptions options, std::shared_ptr<ProbeLink> link)
    : m_options(std::move(options))
    , m_link(std::move(link))
{
    m_selection = selectBackend(m_options.environment ? *m_options.environment : Environment::fromProcess(), x11Built());
}

ProbeWorker::~ProbeWorker() = default;

double ProbeWorker::clockNow() const
{
    return m_options.clock ? m_options.clock() : ps::monotonicSeconds();
}

int ProbeWorker::intervalMs() const
{
    return std::max(100, m_visible ? m_options.visibleIntervalMs : m_options.backgroundIntervalMs);
}

void ProbeWorker::activate(quint64 generation, bool visible)
{
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &ProbeWorker::probe);
    }
    m_generation = generation;
    m_visible = visible;
    m_active = true;
    m_final = false;
    m_failures = 0;
    m_urgent = true;
    probe();
}

void ProbeWorker::setVisible(bool visible)
{
    if (visible == m_visible)
        return;
    m_visible = visible;
    if (!m_active || m_final)
        return;
    const double since = clockNow() - m_lastProbe;
    if (visible && !(since < 1.0))
        probe();
    else
        schedule();
}

void ProbeWorker::pause()
{
    m_active = false;
    if (m_timer)
        m_timer->stop();
}

void ProbeWorker::shutdown()
{
    m_active = false;
    if (m_timer)
        m_timer->stop();
    m_x11.reset();
    QThread::currentThread()->quit();
}

void ProbeWorker::schedule()
{
    if (!m_timer || !m_active || m_final)
        return;
    int wait = intervalMs();
    if (m_failures > 0)
        wait = std::min(std::max(wait, m_options.maxBackoffMs), wait << std::min(m_failures, 4));
    // Count from the last request, so a cadence change does not postpone it.
    const double since = clockNow() - m_lastProbe;
    if (std::isfinite(since) && since > 0)
        wait = std::max(0, wait - static_cast<int>(since * 1000.0));
    m_timer->start(wait);
}

Query ProbeWorker::runQuery(const Selection& selection)
{
    if (m_options.query) {
        QueryContext ctx {selection, m_options.limits, m_link->cancelFd, m_options.procRoot};
        return m_options.query(ctx);
    }
    if (selection.status != Status::Ok)
        return fail(selection.status, selection.detail);
    switch (selection.backend) {
    case Backend::Hyprland:
        return queryHyprland(selection.address, m_options.limits, m_link->cancelFd, m_options.procRoot);
    case Backend::X11:
#ifdef BEX_HAVE_X11
        if (!m_x11)
            m_x11 = std::make_unique<X11Client>();
        return m_x11->query(selection.address, m_options.limits, m_link->cancelFd, m_options.procRoot);
#else
        break;
#endif
    case Backend::None: break;
    }
    return fail(Status::Unsupported, QStringLiteral("No window list is available"));
}

void ProbeWorker::probe()
{
    if (!m_active || m_final)
        return;
    if (m_link->outstanding.load() != 0) {
        // The UI has not taken the previous result yet (after a resume it is
        // about to drop it as obsolete): ask again shortly.
        if (m_timer)
            m_timer->start(m_urgent ? 50 : intervalMs());
        return;
    }
    m_urgent = false;
    auto r = std::make_shared<Result>();
    r->generation = m_generation;
    r->sequence = ++m_sequence;
    r->backend = m_selection.backend;
    r->requested = clockNow();
    const Query q = runQuery(m_selection);
    r->status = q.status;
    r->detail = q.detail;
    r->windows = q.windows;
    r->withoutPid = q.withoutPid;
    if (q.status == Status::Ok)
        resolveOwners(q.pids, r->requested, m_options.procRoot, m_options.clockTicks, *r);
    r->time = clockNow();
    // Valid for two missed requests plus one deadline.
    r->validFor = (2.0 * intervalMs() + m_options.limits.deadlineMs) / 1000.0;
    m_lastProbe = r->time;
    m_failures = r->ok() ? 0 : m_failures + 1;
    if (isFinal(r->status) || r->status == Status::Cancelled)
        m_final = true;
    m_link->outstanding.store(r->sequence);
    Q_EMIT resultReady(std::move(r));
    schedule();
}

// ---------------------------------------------------------------------------

WindowProbe::WindowProbe(ProbeOptions options, QObject* parent)
    : QObject(parent)
    , m_link(std::make_shared<ProbeLink>())
{
    qRegisterMetaType<windows::ResultPtr>();
    m_clock = options.clock;
    m_thread = new QThread; // deliberately unparented, see stop()
    m_thread->setObjectName(QStringLiteral("bex-windows"));
    auto* worker = new ProbeWorker(std::move(options), m_link);
    worker->moveToThread(m_thread);
    m_worker = worker;
    connect(m_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &ProbeWorker::resultReady, this, &WindowProbe::onWorkerResult, Qt::QueuedConnection);
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &WindowProbe::retryStart);
    startThread();
}

// As Collector::startThread(): a refused thread is only logged by Qt.
bool WindowProbe::startThread()
{
    m_thread->start();
    const bool running = m_thread->isRunning();
    m_startFailures = running ? 0 : std::min(m_startFailures + 1, 6);
    m_startError = running ? QString()
                           : QStringLiteral("Could not start the window-list thread: the system refused to create a "
                                            "thread (a process or thread limit may have been reached)");
    return running;
}

void WindowProbe::retryStart()
{
    if (m_stopped || !m_thread || m_thread->isRunning() || !m_started || m_paused)
        return;
    if (startThread()) {
        activateWorker();
        return;
    }
    reportStartFailure();
    m_retryTimer->start(std::min(30000, 1000 << (m_startFailures - 1)));
}

void WindowProbe::activateWorker()
{
    if (!m_worker || !m_thread || !m_thread->isRunning())
        return;
    QMetaObject::invokeMethod(m_worker, "activate", Qt::QueuedConnection, Q_ARG(quint64, m_generation),
                              Q_ARG(bool, m_visible));
}

// Delivered like a worker result (queued, current generation only), so the
// view says why there is no window list instead of waiting for an answer.
void WindowProbe::reportStartFailure()
{
    auto r = std::make_shared<Result>();
    r->status = Status::Failed;
    r->detail = m_startError;
    r->generation = m_generation;
    r->requested = m_clock ? m_clock() : ps::monotonicSeconds();
    r->time = r->requested;
    const quint64 generation = m_generation;
    QMetaObject::invokeMethod(
        this,
        [this, generation, r = ResultPtr(std::move(r))]() mutable {
            if (!m_stopped && !m_paused && generation == m_generation && !isRunning())
                Q_EMIT resultReady(std::move(r));
        },
        Qt::QueuedConnection);
}

WindowProbe::~WindowProbe()
{
    stop();
}

bool WindowProbe::isRunning() const
{
    return !m_stopped && m_thread && m_thread->isRunning();
}

void WindowProbe::start(bool visible)
{
    if (m_stopped)
        return;
    m_visible = visible;
    m_started = true;
    if (m_paused || !m_worker)
        return;
    if (!m_thread->isRunning()) {
        reportStartFailure();
        if (!m_retryTimer->isActive())
            m_retryTimer->start(std::min(30000, 1000 << std::max(0, m_startFailures - 1)));
        return;
    }
    activateWorker();
}

void WindowProbe::setVisible(bool visible)
{
    if (visible == m_visible || m_stopped)
        return;
    m_visible = visible;
    if (m_worker && m_started && m_thread && m_thread->isRunning())
        QMetaObject::invokeMethod(m_worker, "setVisible", Qt::QueuedConnection, Q_ARG(bool, visible));
}

void WindowProbe::setPaused(bool paused)
{
    if (paused == m_paused || m_stopped)
        return;
    m_paused = paused;
    if (!m_worker || !m_started)
        return;
    if (!m_thread->isRunning()) {
        // Never started: resuming retries at once, pausing stops retrying.
        m_retryTimer->stop();
        if (!paused) {
            ++m_generation;
            retryStart();
        }
        return;
    }
    if (paused) {
        QMetaObject::invokeMethod(m_worker, "pause", Qt::QueuedConnection);
    } else {
        // Anything asked before the pause is now obsolete.
        ++m_generation;
        activateWorker();
    }
}

void WindowProbe::onWorkerResult(windows::ResultPtr result)
{
    if (result)
        m_link->acknowledge(result->sequence);
    if (m_stopped || !result || m_paused || result->generation != m_generation)
        return;
    Q_EMIT resultReady(std::move(result));
}

bool WindowProbe::stop(int timeoutMs)
{
    if (!m_stopped) {
        m_stopped = true;
        ++m_generation;
        if (m_worker)
            disconnect(m_worker, nullptr, this, nullptr);
    }
    if (m_retryTimer)
        m_retryTimer->stop();
    if (!m_thread)
        return true;
    if (!m_thread->isRunning() && !m_thread->isFinished()) {
        // Never started: no event loop will delete the worker.
        delete m_worker.data();
        delete m_thread;
        m_thread = nullptr;
        m_worker = nullptr;
        return true;
    }
    if (m_thread->isRunning()) {
        m_link->cancel(); // interrupts a request waiting on the compositor
        if (m_worker)
            QMetaObject::invokeMethod(m_worker, "shutdown", Qt::QueuedConnection);
        else
            m_thread->quit();
        if (!m_thread->wait(timeoutMs)) {
            // Stuck outside a cancellable wait (xcb_connect on a hung X
            // server): abandoned as in Collector::stop().
            qWarning("bex: window probe thread did not stop within %d ms; abandoning it", timeoutMs);
            QThread* thread = m_thread;
            m_worker = nullptr;
            m_thread = nullptr;
            connect(thread, &QThread::finished, thread, &QObject::deleteLater);
            if (thread->isFinished())
                thread->deleteLater();
            return false;
        }
    }
    delete m_thread;
    m_thread = nullptr;
    m_worker = nullptr;
    return true;
}

} // namespace windows
