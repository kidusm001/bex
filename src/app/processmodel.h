// Read-only, grouped process model. Top-level rows are three fixed groups
// (Apps, Background processes, Kernel processes); each process is a child of
// exactly one, keyed by (PID, start time). Rows are updated in place and
// moved between groups, never reset, so selection, expansion and scroll
// position survive refreshes and a reused PID shows up as a different row.
#pragma once

#include "core/snapshot.h"

#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

struct ProcessKey {
    int pid = 0;
    uint64_t startTicks = 0;
    bool operator==(const ProcessKey& o) const { return pid == o.pid && startTicks == o.startTicks; }
};

struct ProcessKeyHash {
    std::size_t operator()(const ProcessKey& k) const
    {
        return std::hash<uint64_t>()((static_cast<uint64_t>(static_cast<uint32_t>(k.pid)) << 40) ^ k.startTicks);
    }
};

// How user-space processes are split between Apps and Background. Only a
// current window list (Detected) puts anything in Apps; otherwise every
// user-space process is shown as unclassified, never as background.
struct ProcessGrouping {
    enum class State { Unavailable, Pending, Detected };
    State state = State::Unavailable;
    QString source; // where the window list came from, e.g. "Hyprland"
    QString reason; // a sentence for the Apps header tooltip and the page note
    std::unordered_set<ProcessKey, ProcessKeyHash> apps; // own a window
    bool operator==(const ProcessGrouping& o) const
    {
        return state == o.state && source == o.source && reason == o.reason && apps == o.apps;
    }
};

class ProcessModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { Name, Pid, User, Cpu, Memory, Threads, State, Nice, IoRead, IoWrite, Age, Command, ColumnCount };
    enum Group { Apps, Background, Kernel, GroupCount };
    enum Role {
        SortRole = Qt::UserRole + 1, // double (NaN when unavailable) or lower-case text; group rows: group
        KernelThreadRole,
        FilterTextRole, // pid, name, user and command joined for filtering
        IsGroupRole,    // bool: a group header row
        GroupRole,      // int Group, for group rows and processes alike
        PidRole,        // int, processes only
        StartTicksRole, // qulonglong, processes only
        GroupTitleRole, // group rows: the title without a count
        GroupSizeRole,  // group rows: number of processes in the group
        CountedRole,    // group rows: whether the title is followed by a count
    };

    explicit ProcessModel(QObject* parent = nullptr);

    // Replaces the processes, keeping the current grouping.
    void setProcesses(const std::vector<ps::ProcessInfo>& processes);
    // Regroups the current processes (and later ones) by `grouping`.
    void setGrouping(ProcessGrouping grouping);
    const ProcessGrouping& grouping() const { return m_grouping; }
    void clear();

    int processCount() const;
    int groupSize(Group group) const { return static_cast<int>(m_groups[static_cast<std::size_t>(group)].size()); }
    QModelIndex groupIndex(Group group, int column = 0) const;
    // The process shown at `index`, or nullptr for group rows.
    const ps::ProcessInfo* process(const QModelIndex& index) const;
    static Group groupFor(const ps::ProcessInfo& process, const ProcessGrouping& grouping);
    static QString groupTitle(Group group, const ProcessGrouping& grouping);
    static bool groupCounted(Group group, const ProcessGrouping& grouping);
    static QString groupToolTip(Group group, const ProcessGrouping& grouping);

    // Stale: rows (identity, grouping, selection, order) stay, but
    // per-sample values (CPU, memory, threads, state, nice, I/O, age)
    // display as unavailable.
    void setStale(bool stale);
    bool isStale() const { return m_stale; }
    static bool isSampledColumn(int column);

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    bool hasChildren(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
    // internalId of a group row; process rows carry their group's index.
    static constexpr quintptr kGroupRow = GroupCount;
    static ProcessKey keyOf(const ps::ProcessInfo& p) { return {p.pid, p.startTicks}; }
    std::vector<ps::ProcessInfo>& rows(int group) { return m_groups[static_cast<std::size_t>(group)]; }
    const std::vector<ps::ProcessInfo>& rows(int group) const { return m_groups[static_cast<std::size_t>(group)]; }
    QVariant groupData(int group, int column, int role) const;
    // Removes, moves, updates and inserts so the groups hold `processes`
    // (or, when null, the current rows) grouped by m_grouping.
    void apply(const std::vector<ps::ProcessInfo>* processes);
    void emitGroupRowsChanged();

    std::array<std::vector<ps::ProcessInfo>, GroupCount> m_groups;
    ProcessGrouping m_grouping;
    bool m_stale = false;
};

// Sorts processes within each group (numerically; unavailable values below
// every real value; ties by PID) while the groups keep their order in both
// directions, and filters processes on PID, name, user and command line.
// Group rows always stay; while filtering their titles count matches
// ("Apps (1 of 3)").
class ProcessFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ProcessFilterModel(QObject* parent = nullptr);

    void setFilterString(const QString& text);
    QString filterString() const { return m_filter; }
    bool isFiltering() const { return !m_filter.isEmpty(); }
    QModelIndex groupIndex(ProcessModel::Group group) const;
    int visibleInGroup(ProcessModel::Group group) const;
    int visibleProcessCount() const;

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

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
};
