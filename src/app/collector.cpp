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
    m_thread->start();
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
    QMetaObject::invokeMethod(m_worker, "activate", Qt::QueuedConnection, Q_ARG(int, m_intervalMs),
                              Q_ARG(quint64, m_generation));
}

void Collector::setIntervalMs(int intervalMs)
{
    if (m_stopped)
        return;
    m_intervalMs = std::clamp(intervalMs, kMinIntervalMs, kMaxIntervalMs);
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "setIntervalMs", Qt::QueuedConnection, Q_ARG(int, m_intervalMs));
}

void Collector::setPaused(bool paused)
{
    if (paused == m_paused || m_stopped)
        return;
    m_paused = paused;
    if (!m_worker || !m_started)
        return;
    if (paused) {
        QMetaObject::invokeMethod(m_worker, "pause", Qt::QueuedConnection);
    } else {
        // A new generation makes any snapshot still in flight from before the
        // pause stale; activation also rebases every counter.
        ++m_generation;
        QMetaObject::invokeMethod(m_worker, "activate", Qt::QueuedConnection, Q_ARG(int, m_intervalMs),
                                  Q_ARG(quint64, m_generation));
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
    if (!m_thread)
        return true;
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
