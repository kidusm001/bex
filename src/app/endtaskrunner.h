// Runs one end-task request (ps::endtask::send, or a replacement injected by
// tests) on its own thread, so the UI thread never reads /proc. At most one
// request runs at a time.
#pragma once

#include "core/endtask.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <thread>

class EndTaskRunner : public QObject {
    Q_OBJECT
public:
    // Runs on the worker thread.
    using Backend = std::function<ps::endtask::Result(const ps::endtask::Target&, ps::endtask::Signal)>;
    // Called on this object's thread when a started request ends.
    using Done = std::function<void(const ps::endtask::Target&, ps::endtask::Signal, const ps::endtask::Result&)>;

    EndTaskRunner(Backend backend, Done done, QObject* parent = nullptr);
    ~EndTaskRunner() override;
    EndTaskRunner(const EndTaskRunner&) = delete;
    EndTaskRunner& operator=(const EndTaskRunner&) = delete;

    bool isBusy() const { return m_job != nullptr; }
    // False, with a reason, if a request is running or its thread cannot be
    // created; nothing is sent then.
    bool start(const ps::endtask::Target& target, ps::endtask::Signal signal, QString* error = nullptr);
    // Waits up to timeoutMs for a running request, then abandons it: it may
    // still finish (the user confirmed it), but `done` is not called for it.
    // True when none is left running.
    bool shutdown(int timeoutMs);

private:
    struct Job;
    void onJobDone(const std::shared_ptr<Job>& job);

    Backend m_backend;
    Done m_done;
    std::shared_ptr<Job> m_job;
    std::thread m_thread;
};
