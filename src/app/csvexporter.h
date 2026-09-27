// Writes a CSV copy of the metric history on one owned worker thread, so a
// slow or stalled destination (a hung network mount, a failing disk) never
// blocks the UI. At most one export runs at a time.
#pragma once

#include "core/history.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <string>
#include <thread>

class CsvExporter : public QObject {
    Q_OBJECT
public:
    // Saves data to path; returns false and sets error on failure. Runs on
    // the worker thread. Replaceable for tests (for example a writer that
    // blocks until released).
    using Writer = std::function<bool(const std::string& data, const QString& path, QString* error)>;

    explicit CsvExporter(Writer writer = {}, QObject* parent = nullptr);
    ~CsvExporter() override;
    CsvExporter(const CsvExporter&) = delete;
    CsvExporter& operator=(const CsvExporter&) = delete;

    bool isBusy() const { return m_job != nullptr; }
    QString currentPath() const;

    // Serializes and saves the given (private) history copy on the worker.
    // Returns false, sets error and stays idle (so it can be retried) if an
    // export is already running or the worker thread cannot be created (for
    // example at the process or thread limit). finished() is emitted on this
    // object's thread when a started job ends.
    bool start(ps::History history, double unixOffset, const QString& path, QString* error = nullptr);

    // Waits up to timeoutMs for a running export. A job still blocked after
    // that is abandoned: its thread is detached, keeps only its own data and
    // its result is discarded (the destination file is written atomically,
    // so it is either complete or untouched). Returns true when no export is
    // left running. finished() is not emitted for a job ended this way.
    bool shutdown(int timeoutMs);

    // Default writer: QSaveFile (temporary file + rename), reporting errors.
    static bool saveAtomically(const std::string& data, const QString& path, QString* error);

Q_SIGNALS:
    void finished(bool ok, const QString& path, const QString& error, qulonglong samples);

private:
    struct Job;
    void onJobDone(const std::shared_ptr<Job>& job);

    Writer m_writer;
    std::shared_ptr<Job> m_job;
    std::thread m_thread;
};
