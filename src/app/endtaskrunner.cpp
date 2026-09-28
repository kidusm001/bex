#include "app/endtaskrunner.h"

#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>

struct EndTaskRunner::Job {
    std::mutex mutex;
    std::condition_variable cv;
    ps::endtask::Target target;
    ps::endtask::Signal signal = ps::endtask::Signal::Terminate;
    // Guarded by mutex; cleared when the runner stops waiting, so an
    // abandoned worker never touches it again.
    EndTaskRunner* receiver = nullptr;
    bool done = false;
    ps::endtask::Result result;
};

EndTaskRunner::EndTaskRunner(Backend backend, Done done, QObject* parent)
    : QObject(parent)
    , m_backend(std::move(backend))
    , m_done(std::move(done))
{
}

EndTaskRunner::~EndTaskRunner()
{
    shutdown(0);
}

bool EndTaskRunner::start(const ps::endtask::Target& target, ps::endtask::Signal signal, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };
    if (m_job)
        return fail(QStringLiteral("Another end task request is still running."));
    if (!m_backend)
        return fail(QStringLiteral("Ending tasks is unavailable."));
    std::shared_ptr<Job> job;
    try {
        job = std::make_shared<Job>();
        job->target = target;
    } catch (const std::bad_alloc&) {
        return fail(QStringLiteral("Out of memory."));
    }
    job->signal = signal;
    job->receiver = this;
    try {
        m_thread = std::thread([job, backend = m_backend] {
            ps::endtask::Result result;
            try {
                result = backend(job->target, job->signal);
            } catch (...) {
                result = {}; // Failed: nothing is known to have been sent
            }
            const std::lock_guard lock(job->mutex);
            job->result = result;
            job->done = true;
            job->cv.notify_all();
            // Posted under the mutex: the receiver cannot be destroyed
            // meanwhile (shutdown takes the mutex first), and Qt drops queued
            // calls to a destroyed receiver.
            if (EndTaskRunner* receiver = job->receiver)
                QMetaObject::invokeMethod(receiver, [receiver, job] { receiver->onJobDone(job); }, Qt::QueuedConnection);
        });
    } catch (const std::system_error& e) {
        return fail(QStringLiteral("Its worker thread could not start: %1.")
                        .arg(QString::fromLocal8Bit(e.code().message().c_str())));
    } catch (const std::bad_alloc&) {
        return fail(QStringLiteral("Out of memory."));
    }
    m_job = std::move(job);
    return true;
}

void EndTaskRunner::onJobDone(const std::shared_ptr<Job>& job)
{
    if (job != m_job)
        return; // abandoned by shutdown()
    if (m_thread.joinable())
        m_thread.join();
    m_job.reset();
    ps::endtask::Result result;
    {
        const std::lock_guard lock(job->mutex);
        result = job->result;
    }
    if (m_done)
        m_done(job->target, job->signal, result);
}

bool EndTaskRunner::shutdown(int timeoutMs)
{
    if (!m_job)
        return true;
    bool done = false;
    {
        std::unique_lock lock(m_job->mutex);
        m_job->cv.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)), [&] { return m_job->done; });
        m_job->receiver = nullptr;
        done = m_job->done;
    }
    if (m_thread.joinable()) {
        if (done)
            m_thread.join();
        else
            m_thread.detach(); // blocked in the kernel; it holds only its own data
    }
    m_job.reset();
    return done;
}
