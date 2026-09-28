#include "app/collector.h"

#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>

CollectorWorker::CollectorWorker(ps::SamplerOptions options, std::shared_ptr<CollectorLink> link)
    : m_options(std::move(options))
    , m_link(std::move(link))
{
}

CollectorWorker::~CollectorWorker() = default;

void CollectorWorker::activate(int intervalMs, quint64 generation)
{
    if (!m_sampler)
        m_sampler = std::make_unique<ps::Sampler>(m_options);
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, &CollectorWorker::tick);
    }
    m_generation = generation;
    m_intervalMs = intervalMs;
    m_gapReferenceMs = intervalMs;
    m_sampler->resetBaselines();
    m_timer->start(m_intervalMs);
    tick();
}

void CollectorWorker::setIntervalMs(int intervalMs)
{
    m_gapReferenceMs = std::max(m_gapReferenceMs, intervalMs);
    m_intervalMs = intervalMs;
    if (m_timer && m_timer->isActive())
        m_timer->start(m_intervalMs);
}

void CollectorWorker::pause()
{
    if (m_timer)
        m_timer->stop();
}

void CollectorWorker::shutdown()
{
    if (m_timer)
        m_timer->stop();
    m_sampler.reset();
    QThread::currentThread()->quit();
}

void CollectorWorker::tick()
{
    if (!m_sampler)
        return;
    if (m_link->outstanding.load() != 0) {
        // The UI has not taken the previous snapshot yet. Skip; the sampler
        // will report the resulting gap honestly.
        m_gapReferenceMs = std::max(m_gapReferenceMs, m_intervalMs);
        return;
    }
    auto snap = std::make_shared<ps::Snapshot>(
        m_sampler->sample(ps::monotonicSeconds(), m_gapReferenceMs / 1000.0));
    snap->generation = m_generation;
    m_gapReferenceMs = m_intervalMs;
    m_link->outstanding.store(snap->sequence);
    Q_EMIT snapshotReady(std::move(snap));
}

// ---------------------------------------------------------------------------

Collector::Collector(ps::SamplerOptions options, QObject* parent)
    : QObject(parent)
    , m_link(std::make_shared<CollectorLink>())
{
    qRegisterMetaType<ps::SnapshotPtr>();
    m_thread = new QThread; // deliberately unparented, see stop()
    m_thread->setObjectName(QStringLiteral("pulsescope-collector"));
    auto* worker = new CollectorWorker(std::move(options), m_link);
    worker->moveToThread(m_thread);
    m_worker = worker;
    connect(m_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &CollectorWorker::snapshotReady, this, &Collector::onWorkerSnapshot, Qt::QueuedConnection);
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &Collector::retryStart);
    startThread();
}

// QThread::start() only logs a warning when the system refuses a new thread
// (EAGAIN at RLIMIT_NPROC or the cgroup's pids.max); the thread is then just
// not running, and the worker stays unstarted until a retry succeeds.
bool Collector::startThread()
{
    m_thread->start();
    const bool running = m_thread->isRunning();
    m_startFailures = running ? 0 : std::min(m_startFailures + 1, 6);
    const QString error = running ? QString()
                                  : QStringLiteral("The collector thread could not be started: the system refused to "
                                                   "create a thread (a process or thread limit may have been reached)");
    if (error != m_startError) {
        m_startError = error;
        Q_EMIT startErrorChanged();
    }
    return running;
}

void Collector::retryStart()
{
    if (m_stopped || !m_thread || m_thread->isRunning())
        return;
    if (startThread()) {
        if (m_started && !m_paused)
            activateWorker();
    } else if (m_started && !m_paused) {
        m_retryTimer->start(std::min(30000, 1000 << (m_startFailures - 1)));
    }
}

void Collector::activateWorker()
{
    if (!m_worker || !m_thread || !m_thread->isRunning())
        return;
    QMetaObject::invokeMethod(m_worker, "activate", Qt::QueuedConnection, Q_ARG(int, m_intervalMs),
                              Q_ARG(quint64, m_generation));
}

Collector::~Collector()
{
    stop();
}

bool Collector::isRunning() const
{
    return !m_stopped && m_thread && m_thread->isRunning();
}

void Collector::start(int intervalMs)
{
    if (m_stopped)
        return;
    m_intervalMs = std::clamp(intervalMs, kMinIntervalMs, kMaxIntervalMs);
    m_started = true;
    if (m_paused || !m_worker)
        return; // setPaused(false) activates the worker later
    if (!m_thread->isRunning()) {
        if (!m_retryTimer->isActive())
            m_retryTimer->start(std::min(30000, 1000 << std::max(0, m_startFailures - 1)));
        return; // activated once a retry starts the thread
    }
    activateWorker();
}

void Collector::setIntervalMs(int intervalMs)
{
    if (m_stopped)
        return;
    m_intervalMs = std::clamp(intervalMs, kMinIntervalMs, kMaxIntervalMs);
    if (m_worker && m_thread && m_thread->isRunning())
        QMetaObject::invokeMethod(m_worker, "setIntervalMs", Qt::QueuedConnection, Q_ARG(int, m_intervalMs));
}

void Collector::setPaused(bool paused)
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
        // A new generation makes any snapshot still in flight from before the
        // pause stale; activation also rebases every counter.
        ++m_generation;
        activateWorker();
    }
}

void Collector::onWorkerSnapshot(ps::SnapshotPtr snapshot)
{
    if (snapshot)
        m_link->acknowledge(snapshot->sequence);
    if (m_stopped || !snapshot || m_paused || snapshot->generation != m_generation)
        return;
    Q_EMIT snapshotReady(std::move(snapshot));
}

bool Collector::stop(int timeoutMs)
{
    if (!m_stopped) {
        // Invalidate first so nothing already queued can be delivered.
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
        // The thread never started, so its event loop cannot delete the
        // worker; nothing else references it.
        delete m_worker.data();
        delete m_thread;
        m_thread = nullptr;
        m_worker = nullptr;
        return true;
    }
    if (m_thread->isRunning()) {
        if (m_worker)
            QMetaObject::invokeMethod(m_worker, "shutdown", Qt::QueuedConnection);
        else
            m_thread->quit();
        if (!m_thread->wait(timeoutMs)) {
            // The worker is blocked inside a kernel call. Destroying a running
            // QThread would abort and waiting longer would hang the UI, so the
            // thread is abandoned: it still has the queued shutdown and exits
            // when the call returns, deleting the worker (the finished -> deleteLater
            // connection made in the constructor). The QThread object, which lives on this
            // thread, is then deleted by this thread's event loop. The worker
            // only references its own state and the shared link.
            qWarning("PulseScope: collector thread did not stop within %d ms; abandoning it", timeoutMs);
            QThread* thread = m_thread;
            m_worker = nullptr;
            m_thread = nullptr;
            // Connect before checking, so a thread finishing right now is
            // never missed. If it is already finishing, the signal may or may
            // not reach this connection; deleteLater() twice is harmless, and
            // ~QThread waits for a thread that is still inside its finish.
            connect(thread, &QThread::finished, thread, &QObject::deleteLater);
            if (thread->isFinished())
                thread->deleteLater();
            return false;
        }
    }
    delete m_thread; // finished: the worker was deleted via deleteLater
    m_thread = nullptr;
    m_worker = nullptr;
    return true;
}
