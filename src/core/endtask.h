// Ending one process, as a task manager does: SIGTERM ("End task") or
// SIGKILL ("Force end task") to exactly the process the user chose, never
// its children, group or other processes with the same name. No Qt.
//
// The signal goes through a pidfd (pidfd_open, pidfd_send_signal), opened
// before the live /proc identity is checked, so a PID reused after the check
// can never receive it. Without pidfd support nothing is sent: there is no
// fallback to kill(pid).
#pragma once

#include "core/snapshot.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ps::endtask {

enum class Signal { Terminate, Kill }; // SIGTERM, SIGKILL

// The process as it was listed when the user chose it: a copy, never a
// pointer or row in a model that may refresh.
struct Target {
    int pid = 0;
    uint64_t startTicks = 0; // with pid, the identity
    std::string name;        // kernel comm as listed (ProcessInfo::name)
    uint32_t uid = 0;        // real UID as listed
    bool uidKnown = false;
    bool kernelThread = false;
};

Target targetOf(const ProcessInfo& process);

enum class Outcome {
    Sent,        // the kernel accepted the signal; the process may not have ended yet
    Exited,      // it had already exited (gone, or a zombie waiting for its parent)
    Changed,     // the PID now names another process, or its name or owner changed
    NotOwned,    // another user's process, or its owner is unknown
    Protected,   // PID 1, PulseScope itself, or a kernel thread
    Invalid,     // no usable identity (PID 0 or negative), or not a Signal value
    Denied,      // the kernel refused the signal (EPERM)
    Unsupported, // no pidfd support, or /proc is not this PID namespace's procfs
    Failed,      // /proc unreadable, truncated or unparsable; out of descriptors
};

struct Result {
    Outcome outcome = Outcome::Failed;
    int error = 0;    // errno behind the outcome, 0 if none
    char state = '?'; // live /proc state when checked ('T' or 't': stopped)
    bool kernelThread = false; // the live stat flags say kernel thread
};

// Checks the listed identity alone, without I/O. Sent means it may be tried.
Outcome precheck(const Target& target, uint32_t ownUid, int selfPid);

// Test seams; the defaults are the real system calls and account.
struct Hooks {
    // -1 with errno set on failure. Default: pidfd_open(pid, 0).
    std::function<int(int pid)> pidfdOpen;
    // 0, or -1 with errno set. Default: pidfd_send_signal(pidfd, sig, nullptr, 0).
    std::function<int(int pidfd, int sig)> sendSignal;
    // Called after every check passed, just before the signal is sent.
    std::function<void()> beforeSend;
    std::optional<uint32_t> ownUid; // default: getuid()
};

// Sends `signal` to `target` if, on this system's /proc (always "/proc",
// never a configurable root), it is still the same process: same PID, start
// time and name, not a kernel thread, not exited, and owned by this account
// (live real UID equal to the listed one and to ownUid). Nothing is sent
// unless the outcome is Sent. Synchronous: it reads /proc, so the UI calls it
// on a worker thread (EndTaskRunner).
Result send(const Target& target, Signal signal, const Hooks& hooks = {});

// One or two sentences for the user: what was sent, or why nothing was.
std::string describe(const Result& result, const Target& target, Signal signal);

} // namespace ps::endtask
