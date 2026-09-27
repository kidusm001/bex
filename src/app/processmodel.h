// Read-only process table model. Rows are keyed by (PID, start time) and
// updated in place so selection and scroll position survive refreshes and a
// reused PID shows up as a different row.
#pragma once

#include "core/snapshot.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <cstdint>
#include <unordered_map>
#include <vector>

class ProcessModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Pid, Name, User, Cpu, Memory, Threads, State, Nice, IoRead, IoWrite, Age, Command, ColumnCount };
    enum Role {
        SortRole = Qt::UserRole + 1, // double (NaN when unavailable) or lower-case text
        KernelThreadRole,
        FilterTextRole, // pid, name, user and command joined for filtering
    };

    explicit ProcessModel(QObject* parent = nullptr);

    void setProcesses(const std::vector<ps::ProcessInfo>& processes);
    void clear();
    const ps::ProcessInfo& process(int row) const { return m_rows[static_cast<std::size_t>(row)]; }
    // Stale: rows (identity, selection, order) stay, but per-sample values
    // (CPU, memory, threads, state, nice, I/O, age) display as unavailable.
    void setStale(bool stale);
    bool isStale() const { return m_stale; }
    static bool isSampledColumn(int column);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    struct Key {
        int pid;
        uint64_t start;
        bool operator==(const Key& o) const { return pid == o.pid && start == o.start; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const
        {
            return std::hash<uint64_t>()((static_cast<uint64_t>(static_cast<uint32_t>(k.pid)) << 40) ^ k.start);
        }
    };
    static Key keyOf(const ps::ProcessInfo& p) { return {p.pid, p.startTicks}; }

    std::vector<ps::ProcessInfo> m_rows;
    bool m_stale = false;
};

// Numeric sorting (unavailable values sort below every real value) and
// case-insensitive filtering on PID, name, user and command line.
class ProcessFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ProcessFilterModel(QObject* parent = nullptr);

    void setFilterString(const QString& text);
    QString filterString() const { return m_filter; }
    void setShowKernelThreads(bool show);
    bool showKernelThreads() const { return m_showKernel; }

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    template <typename F>
    void changeRowsFilter(F&& apply)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        apply();
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        apply();
        invalidateRowsFilter();
#endif
    }

    QString m_filter;
    bool m_showKernel = false;
};
