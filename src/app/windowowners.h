// Which processes own a window, for the process view's Apps group.
//
// A probe thread asks the desktop for its window list (Hyprland's IPC socket,
// or EWMH _NET_CLIENT_LIST with XRes client PIDs on an X11 session), keeps
// only each window's process ID, and pins every PID to its process identity
// (PID + start time from /proc) at that moment, so a PID reused later never
// inherits the classification. Nothing else about a window (title, class,
// workspace) is kept or logged.
//
// Other Wayland compositors do not share their window list; the result then
// says so (Status::Unsupported) and the view shows app detection as
// unavailable instead of guessing. XWayland is never taken for the full list.
#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QThread;
class QTimer;

namespace windows {

enum class Backend { None, Hyprland, X11 };

enum class Status {
    Ok,
    Unsupported,      // this desktop offers no window list bex can read
    MissingSession,   // session variables are set but unusable
    NotRunning,       // no socket, or nobody listening on it
    PermissionDenied,
    OtherUser,        // the socket is served by another user
    OtherNamespace,   // the desktop's process IDs are not the ones in /proc
    Timeout,          // no complete reply before the deadline
    Disconnected,     // closed without replying
    Truncated,        // closed in the middle of the reply
    TooLarge,         // reply larger than Limits::maxBytes
    TooMany,          // more windows than Limits::maxWindows
    Malformed,        // not valid JSON, or a record that is not an object
    NotArray,         // a reply, but not a window list
    Cancelled,        // the probe is shutting down
    Failed,           // any other error; see detail
};

// Stable identifier, e.g. "not-running" (diagnostics and tests).
const char* statusId(Status status);
// Retrying cannot help within this run (the environment does not change).
bool isFinal(Status status);
QString backendName(Backend backend);

// The session variables that choose a backend.
struct Environment {
    std::string hyprlandSignature; // HYPRLAND_INSTANCE_SIGNATURE
    std::string runtimeDir;        // XDG_RUNTIME_DIR
    std::string waylandDisplay;    // WAYLAND_DISPLAY
    std::string sessionType;       // XDG_SESSION_TYPE
    std::string display;           // DISPLAY
    static Environment fromProcess();
};

struct Selection {
    Backend backend = Backend::None;
    Status status = Status::Unsupported; // Ok when a backend was chosen
    QString detail;                      // why not, when status != Ok
    std::string address;                 // Hyprland socket path or X11 display
};

// Hyprland when its instance signature is set; otherwise, on any other
// Wayland session, unsupported (XWayland only lists X11 windows); otherwise
// X11 when DISPLAY is set and this build has X11 support.
Selection selectBackend(const Environment& env, bool x11Available);
bool x11Built();

struct Limits {
    int deadlineMs = 400;              // whole request: connect, send, reply
    qsizetype maxBytes = 2 * 1024 * 1024;
    int maxWindows = 1024;
};

// A backend's answer, before PIDs are pinned to processes.
struct Query {
    Status status = Status::Failed;
    QString detail;         // short reason; never window titles or commands
    int windows = 0;        // window records received
    int withoutPid = 0;     // records without a usable process ID
    std::vector<int> pids;  // sorted, unique
};

// Parses a complete `j/clients` reply (the connection was closed by
// Hyprland). Each record's "pid" must be an integer in 1..4194304; others
// count as windows without a PID. Several windows of one process give one PID.
Query parseHyprlandClients(const QByteArray& reply, const Limits& limits);

// Connects to Hyprland's request socket, sends `j/clients` and reads the
// reply, within limits.deadlineMs. The socket must be served by this user
// from this PID namespace (checked with SO_PEERCRED and /proc). cancelFd, if
// not negative, aborts the wait as soon as it becomes readable.
Query queryHyprland(const std::string& socketPath, const Limits& limits, int cancelFd,
                    const std::string& procRoot = "/proc");

struct Owner {
    int pid = 0;
    uint64_t startTicks = 0;
    bool operator==(const Owner& o) const { return pid == o.pid && startTicks == o.startTicks; }
    bool operator<(const Owner& o) const { return pid != o.pid ? pid < o.pid : startTicks < o.startTicks; }
};

struct Result {
    Status status = Status::Failed;
    Backend backend = Backend::None;
    QString detail;
    quint64 generation = 0;
    uint64_t sequence = 0;
    double requested = std::numeric_limits<double>::quiet_NaN(); // clock when the request started
    double time = std::numeric_limits<double>::quiet_NaN();      // clock when it was resolved
    double validFor = 0;   // seconds after `time` this list may be used
    int windows = 0;
    int withoutPid = 0;
    int unresolved = 0;    // PID gone, unreadable, or a kernel thread
    int ambiguous = 0;     // PID started after the request: reused, dropped
    std::vector<Owner> owners; // sorted, unique
    bool ok() const { return status == Status::Ok; }
    // Ok, and every window's owner was identified: only such a list shows
    // which processes own no window. An incomplete one is not used for
    // grouping (describe() says what is missing).
    bool complete() const { return ok() && withoutPid == 0 && unresolved == 0 && ambiguous == 0; }
};
using ResultPtr = std::shared_ptr<const Result>;

// A user-facing sentence for a failed or incomplete result ("" when complete).
QString describe(const Result& result);

// Pins PIDs to (PID, start time) through /proc/PID descriptors. A PID whose
// process started after `requested` (clock seconds on the /proc start-time
// timeline, CLOCK_BOOTTIME) was reused after the list was taken and is
// dropped as ambiguous rather than guessed.
void resolveOwners(const std::vector<int>& pids, double requested, const std::string& procRoot, long clockTicks,
                   Result& out);

// What a query function gets. `cancelFd` becomes readable on shutdown.
struct QueryContext {
    Selection selection;
    Limits limits;
    int cancelFd = -1;
    std::string procRoot;
};
using QueryFunction = std::function<Query(const QueryContext&)>;

struct ProbeOptions {
    bool enabled = true;
    std::optional<Environment> environment; // empty: this process's environment
    std::string procRoot = "/proc";
    long clockTicks = 0; // 0: sysconf(_SC_CLK_TCK)
    Limits limits;
    int visibleIntervalMs = 1500; // the process list is on screen
    int backgroundIntervalMs = 5000;
    int maxBackoffMs = 10000;     // repeated failures slow down to this
    // Seconds on the CLOCK_BOOTTIME timeline (the Snapshot::time clock).
    std::function<double()> clock;
    // Replaces the backend request (runs on the probe thread). Tests only.
    QueryFunction query;
};

// Shared by the facade and the worker: at most one result is ever queued,
// and a cancel descriptor that interrupts a request in progress.
struct ProbeLink {
    ProbeLink();
    ~ProbeLink();
    ProbeLink(const ProbeLink&) = delete;
    ProbeLink& operator=(const ProbeLink&) = delete;

    std::atomic<uint64_t> outstanding {0}; // sequence in flight, 0 = none
    int cancelFd = -1;                     // eventfd

    bool acknowledge(uint64_t sequence)
    {
        uint64_t expected = sequence;
        return sequence != 0 && outstanding.compare_exchange_strong(expected, 0);
    }
    void cancel();
};

class X11Client;

// Lives on the probe thread; only touched through queued invocations.
class ProbeWorker : public QObject {
    Q_OBJECT
public:
    ProbeWorker(ProbeOptions options, std::shared_ptr<ProbeLink> link);
    ~ProbeWorker() override;

public Q_SLOTS:
    // Starts, or restarts after a pause, and asks at once.
    void activate(quint64 generation, bool visible);
    // Visible: the list is on screen, so ask every visibleIntervalMs, and at
    // once if the last answer is older than a second.
    void setVisible(bool visible);
    void pause();
    void shutdown();

Q_SIGNALS:
    void resultReady(windows::ResultPtr result);

private:
    void probe();
    void schedule();
    int intervalMs() const;
    double clockNow() const;
    Query runQuery(const Selection& selection);

    ProbeOptions m_options;
    std::shared_ptr<ProbeLink> m_link;
    Selection m_selection;
    QTimer* m_timer = nullptr;
    std::unique_ptr<X11Client> m_x11;
    quint64 m_generation = 0;
    uint64_t m_sequence = 0;
    bool m_active = false;
    bool m_visible = false;
    bool m_final = false;  // reported a final status in this activation
    bool m_urgent = false; // activated, the first answer is still to come
    int m_failures = 0;
    double m_lastProbe = std::numeric_limits<double>::quiet_NaN();
};

// UI-thread facade that owns the probe thread. Results from a previous
// generation (asked before a pause), arriving while paused, or after stop()
// are dropped.
class WindowProbe : public QObject {
    Q_OBJECT
public:
    explicit WindowProbe(ProbeOptions options, QObject* parent = nullptr);
    ~WindowProbe() override;

    void start(bool visible);
    void setVisible(bool visible);
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }
    bool isStopped() const { return m_stopped; }
    bool isRunning() const;
    // Why the probe thread is not running (the system refused to create it);
    // empty while it runs. While started and not paused, the failure is also
    // delivered as a Status::Failed result, and creation is retried (backing
    // off from 1 s to 30 s, and at once on resume).
    QString startError() const { return m_startError; }
    quint64 generation() const { return m_generation; }
    uint64_t outstandingSequence() const { return m_link->outstanding.load(); }
    // Cancels a request in progress and stops the thread, waiting at most
    // timeoutMs; false if it had to be abandoned (as Collector::stop()).
    bool stop(int timeoutMs = 1000);

Q_SIGNALS:
    void resultReady(windows::ResultPtr result);

private:
    void onWorkerResult(windows::ResultPtr result);
    bool startThread();
    void retryStart();
    void activateWorker();
    void reportStartFailure();

    std::shared_ptr<ProbeLink> m_link;
    QThread* m_thread = nullptr;
    QPointer<ProbeWorker> m_worker;
    QTimer* m_retryTimer = nullptr;
    std::function<double()> m_clock;
    QString m_startError;
    int m_startFailures = 0;
    bool m_visible = false;
    bool m_paused = false;
    bool m_started = false;
    bool m_stopped = false;
    quint64 m_generation = 1;
};

} // namespace windows

Q_DECLARE_METATYPE(windows::ResultPtr)
