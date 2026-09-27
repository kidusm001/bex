// Runs the Sampler on a dedicated thread and delivers immutable snapshots to
// the UI thread through queued signals.
#pragma once

#include "core/sampler.h"
#include "core/snapshot.h"

#include <QObject>
#include <QPointer>
#include <atomic>
#include <cstdint>
#include <memory>

class QThread;
class QTimer;

Q_DECLARE_METATYPE(ps::SnapshotPtr)

// Shared between the facade and the worker: a single delivery permit. The
// worker emits only while no snapshot is outstanding and records the emitted
// snapshot's sequence; the UI thread releases the permit only for that exact
// sequence (whether it delivers or drops the snapshot). Activations and
// generation changes never reset it, so however often collection is started,
// paused or resumed at most one snapshot is ever queued. Skipped ticks show up
// as a longer gap, which the sampler marks as discontinuous.
struct CollectorLink {
    std::atomic<uint64_t> outstanding {0}; // sequence in flight, 0 = none

    bool acknowledge(uint64_t sequence)
    {
        uint64_t expected = sequence;
        return sequence != 0 && outstanding.compare_exchange_strong(expected, 0);
    }
};

// Lives on the collector thread. Only touched through queued invocations.
class CollectorWorker : public QObject {
    Q_OBJECT
public:
    CollectorWorker(ps::SamplerOptions options, std::shared_ptr<CollectorLink> link);
    ~CollectorWorker() override;

public Q_SLOTS:
    // Starts, or restarts after a pause: rebases counters, samples at once
    // (gauges are valid immediately, rates on the next tick) and then ticks.
    void activate(int intervalMs, quint64 generation);
    void setIntervalMs(int intervalMs);
    void pause();
    void shutdown();

Q_SIGNALS:
    void snapshotReady(ps::SnapshotPtr snapshot);

private:
    void tick();

    ps::SamplerOptions m_options;
    std::shared_ptr<CollectorLink> m_link;
    std::unique_ptr<ps::Sampler> m_sampler;
    QTimer* m_timer = nullptr;
    int m_intervalMs = 1000;
    int m_gapReferenceMs = 1000; // largest interval since the last sample
    quint64 m_generation = 0;
};

// UI-thread facade that owns the thread. Snapshots from a previous
// generation (collected before a pause), arriving while paused, or after
// stop() are dropped, so a paused or stopped view never changes.
class Collector : public QObject {
    Q_OBJECT
public:
    explicit Collector(ps::SamplerOptions options = {}, QObject* parent = nullptr);
    ~Collector() override;

    void start(int intervalMs);
    void setIntervalMs(int intervalMs);
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }
    bool isStopped() const { return m_stopped; }
    bool isRunning() const;
    int intervalMs() const { return m_intervalMs; }
    quint64 generation() const { return m_generation; }
    // Sequence of the snapshot currently in flight to this facade, 0 if none.
    uint64_t outstandingSequence() const { return m_link->outstanding.load(); }
    // Stops the thread, waiting at most timeoutMs. Returns false if the
    // worker is stuck in an uninterruptible kernel call; the thread is then
    // abandoned (it exits on its own once the call returns) rather than
    // blocking the UI. See README, "Shutdown".
    bool stop(int timeoutMs = 3000);

    static constexpr int kMinIntervalMs = 250;
    static constexpr int kMaxIntervalMs = 60000;

Q_SIGNALS:
    void snapshotReady(ps::SnapshotPtr snapshot);

private:
    void onWorkerSnapshot(ps::SnapshotPtr snapshot);

    std::shared_ptr<CollectorLink> m_link;
    QThread* m_thread = nullptr;
    QPointer<CollectorWorker> m_worker;
    int m_intervalMs = 1000;
    bool m_paused = false;
    bool m_started = false;
    bool m_stopped = false;
    quint64 m_generation = 1;
};
