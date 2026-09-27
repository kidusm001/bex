#include "app/csvexporter.h"

#include <QMetaObject>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>

struct CsvExporter::Job {
    std::mutex mutex;
    std::condition_variable cv;
    QString path;
    qulonglong samples = 0;
    // Guarded by mutex. The receiver is cleared when the exporter stops
    // waiting, so an abandoned worker never touches it again.
    CsvExporter* receiver = nullptr;
    bool done = false;
    bool ok = false;
    QString error;
};

CsvExporter::CsvExporter(Writer writer, QObject* parent)
    : QObject(parent)
    , m_writer(std::move(writer))
{
}

CsvExporter::~CsvExporter()
{
    // The owner normally calls shutdown() with its own time budget first.
    shutdown(0);
}

QString CsvExporter::currentPath() const
{
    return m_job ? m_job->path : QString();
}

bool CsvExporter::start(ps::History history, double unixOffset, const QString& path, QString* errorOut)
{
    auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };
    if (m_job)
        return fail(QStringLiteral("An export to %1 is still running.").arg(m_job->path));
    // Preparing and creating the worker can fail (no memory, EAGAIN at the
    // thread limit). Nothing is committed until the thread exists, so a
    // failure leaves the exporter idle.
    std::shared_ptr<Job> job;
    try {
        job = std::make_shared<Job>();
    } catch (const std::bad_alloc&) {
        return fail(QStringLiteral("The export could not start: out of memory."));
    }
    job->path = path;
    job->samples = history.size();
    job->receiver = this;
    // The worker owns copies of everything it uses (history, writer, path).
    try {
        m_thread = std::thread([job, writer = m_writer, history = std::move(history), unixOffset]() mutable {
            bool ok = false;
            QString error;
            try {
                const std::string csv = history.toCsv(unixOffset);
                history = ps::History(2, 1.0); // release the copy before the (possibly slow) write
                ok = writer ? writer(csv, job->path, &error) : saveAtomically(csv, job->path, &error);
                if (!ok && error.isEmpty())
                    error = QStringLiteral("unknown error");
            } catch (const std::exception& e) {
                ok = false;
                error = QString::fromLocal8Bit(e.what());
            } catch (...) {
                ok = false;
                error = QStringLiteral("unexpected failure");
            }
            const std::lock_guard lock(job->mutex);
            job->ok = ok;
            job->error = error;
            job->done = true;
            job->cv.notify_all();
            // Posting under the mutex: the receiver cannot be destroyed meanwhile
            // (its shutdown takes the mutex first), and a destroyed receiver's
            // pending queued calls are discarded by Qt.
            if (CsvExporter* receiver = job->receiver)
                QMetaObject::invokeMethod(receiver, [receiver, job] { receiver->onJobDone(job); }, Qt::QueuedConnection);
        });
    } catch (const std::system_error& e) {
        return fail(QStringLiteral("The export could not start its worker thread: %1.")
                        .arg(QString::fromLocal8Bit(e.code().message().c_str())));
    } catch (const std::bad_alloc&) {
        return fail(QStringLiteral("The export could not start: out of memory."));
    }
    m_job = std::move(job);
    return true;
}

void CsvExporter::onJobDone(const std::shared_ptr<Job>& job)
{
    if (job != m_job)
        return; // abandoned by shutdown(); its result is intentionally dropped
    if (m_thread.joinable())
        m_thread.join(); // the worker has finished its work and is returning
    m_job.reset();
    bool ok = false;
    QString error;
    {
        const std::lock_guard lock(job->mutex);
        ok = job->ok;
        error = job->error;
    }
    Q_EMIT finished(ok, job->path, error, job->samples);
}

bool CsvExporter::shutdown(int timeoutMs)
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

bool CsvExporter::saveAtomically(const std::string& data, const QString& path, QString* errorOut)
{
    QString ignored;
    QString* error = errorOut ? errorOut : &ignored;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    if (file.write(data.data(), static_cast<qint64>(data.size())) != static_cast<qint64>(data.size())) {
        *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
