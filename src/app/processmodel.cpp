#include "app/processmodel.h"

#include "core/format.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
const QString kUnavailable = QStringLiteral("\u2014");

QString q(const std::string& s)
{
    return QString::fromStdString(s);
}

QString stateName(char state)
{
    switch (state) {
    case 'R': return QStringLiteral("Running");
    case 'S': return QStringLiteral("Sleeping");
    case 'D': return QStringLiteral("Disk wait");
    case 'Z': return QStringLiteral("Zombie");
    case 'T': return QStringLiteral("Stopped");
    case 't': return QStringLiteral("Traced");
    case 'X': return QStringLiteral("Dead");
    case 'I': return QStringLiteral("Idle");
    case 'P': return QStringLiteral("Parked");
    case 'W': return QStringLiteral("Paging");
    default: return QStringLiteral("Unknown");
    }
}
} // namespace

ProcessModel::ProcessModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int ProcessModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int ProcessModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

void ProcessModel::clear()
{
    if (m_rows.empty())
        return;
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

void ProcessModel::setProcesses(const std::vector<ps::ProcessInfo>& processes)
{
    std::unordered_map<Key, std::size_t, KeyHash> incoming;
    incoming.reserve(processes.size());
    for (std::size_t i = 0; i < processes.size(); ++i)
        incoming.emplace(keyOf(processes[i]), i);

    // Remove vanished rows, merging contiguous runs, from the end so earlier
    // row numbers stay valid.
    int row = static_cast<int>(m_rows.size()) - 1;
    while (row >= 0) {
        if (incoming.count(keyOf(m_rows[static_cast<std::size_t>(row)]))) {
            --row;
            continue;
        }
        int first = row;
        while (first > 0 && !incoming.count(keyOf(m_rows[static_cast<std::size_t>(first - 1)])))
            --first;
        beginRemoveRows({}, first, row);
        m_rows.erase(m_rows.begin() + first, m_rows.begin() + row + 1);
        endRemoveRows();
        row = first - 1;
    }

    // Update survivors in place.
    std::unordered_set<Key, KeyHash> present;
    present.reserve(m_rows.size());
    for (auto& existing : m_rows) {
        const Key k = keyOf(existing);
        existing = processes[incoming.at(k)];
        present.insert(k);
    }
    if (!m_rows.empty())
        Q_EMIT dataChanged(index(0, 0), index(static_cast<int>(m_rows.size()) - 1, ColumnCount - 1));

    // Append new processes.
    std::vector<const ps::ProcessInfo*> added;
    for (const auto& p : processes)
        if (!present.count(keyOf(p)))
            added.push_back(&p);
    if (!added.empty()) {
        const int start = static_cast<int>(m_rows.size());
        beginInsertRows({}, start, start + static_cast<int>(added.size()) - 1);
        for (const auto* p : added)
            m_rows.push_back(*p);
        endInsertRows();
    }
}

QVariant ProcessModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const auto& p = m_rows[static_cast<std::size_t>(index.row())];
    const int col = index.column();

    if (role == KernelThreadRole)
        return p.kernelThread;
    if (role == FilterTextRole)
        return QString::number(p.pid) + QLatin1Char('\n') + q(p.name) + QLatin1Char('\n') + q(p.user)
            + QLatin1Char('\n') + q(p.command);

    if (role == SortRole) {
        switch (col) {
        case Pid: return static_cast<double>(p.pid);
        case Name: return q(p.name).toLower();
        case User: return q(p.user).toLower();
        case Cpu: return p.cpuPercent;
        case Memory: return p.rssBytes;
        case Threads: return static_cast<double>(p.threads);
        case State: return stateName(p.state);
        case Nice: return static_cast<double>(p.nice);
        case IoRead: return p.ioReadBps;
        case IoWrite: return p.ioWriteBps;
        case Age: return p.ageSeconds;
        case Command: return q(p.command).toLower();
        default: return {};
        }
    }

    if (m_stale && isSampledColumn(col)) {
        if (role == Qt::DisplayRole)
            return kUnavailable;
        if (role == Qt::ToolTipRole)
            return QStringLiteral("No recent samples: the current value is unknown");
    }

    if (role == Qt::DisplayRole) {
        switch (col) {
        case Pid: return p.pid;
        case Name: return q(p.name);
        case User: return p.uidKnown ? q(p.user) : kUnavailable;
        case Cpu: return ps::isValid(p.cpuPercent) ? QString::number(p.cpuPercent, 'f', 1) : kUnavailable;
        case Memory: return ps::isValid(p.rssBytes) ? q(ps::fmt::bytes(p.rssBytes)) : kUnavailable;
        case Threads: return p.threads > 0 ? QVariant(p.threads) : QVariant(kUnavailable);
        case State: return stateName(p.state);
        case Nice: return p.nice;
        case IoRead: return ps::isValid(p.ioReadBps) ? q(ps::fmt::rate(p.ioReadBps)) : kUnavailable;
        case IoWrite: return ps::isValid(p.ioWriteBps) ? q(ps::fmt::rate(p.ioWriteBps)) : kUnavailable;
        case Age: return ps::isValid(p.ageSeconds) ? q(ps::fmt::duration(p.ageSeconds)) : kUnavailable;
        case Command: return q(p.command);
        default: return {};
        }
    }

    if (role == Qt::TextAlignmentRole) {
        switch (col) {
        case Pid:
        case Cpu:
        case Memory:
        case Threads:
        case Nice:
        case IoRead:
        case IoWrite:
        case Age: return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        default: return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    if (role == Qt::ToolTipRole) {
        switch (col) {
        case Cpu:
            if (!ps::isValid(p.cpuPercent))
                return QStringLiteral("Measuring: CPU use needs two samples of this process");
            return QStringLiteral("%1% of one logical CPU").arg(p.cpuPercent, 0, 'f', 1);
        case IoRead:
        case IoWrite:
            if (p.kernelThread)
                return QStringLiteral("Kernel threads have no per-process I/O accounting");
            if (p.ioDenied)
                return QStringLiteral("Not permitted: /proc/%1/io is only readable for your own processes").arg(p.pid);
            if (!ps::isValid(col == IoRead ? p.ioReadBps : p.ioWriteBps))
                return QStringLiteral("Measuring: needs two samples of this process");
            return {};
        case Command: return q(p.command);
        case Name:
            return QStringLiteral("PID %1, parent PID %2").arg(p.pid).arg(p.ppid);
        default: return {};
        }
    }
    return {};
}

bool ProcessModel::isSampledColumn(int column)
{
    switch (column) {
    case Cpu:
    case Memory:
    case Threads:
    case State:
    case Nice:
    case IoRead:
    case IoWrite:
    case Age: return true;
    default: return false;
    }
}

void ProcessModel::setStale(bool stale)
{
    if (stale == m_stale)
        return;
    m_stale = stale;
    if (!m_rows.empty())
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1), {Qt::DisplayRole, Qt::ToolTipRole});
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case Pid: return QStringLiteral("PID");
        case Name: return QStringLiteral("Name");
        case User: return QStringLiteral("User");
        case Cpu: return QStringLiteral("CPU %");
        case Memory: return QStringLiteral("Memory (RSS)");
        case Threads: return QStringLiteral("Threads");
        case State: return QStringLiteral("State");
        case Nice: return QStringLiteral("Nice");
        case IoRead: return QStringLiteral("Disk read");
        case IoWrite: return QStringLiteral("Disk write");
        case Age: return QStringLiteral("Running for");
        case Command: return QStringLiteral("Command");
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (section) {
        case Cpu:
            return QStringLiteral("CPU time over the last interval. 100% is one logical CPU fully busy, so a "
                                  "multithreaded process can exceed 100%.");
        case Memory:
            return QStringLiteral("Resident set size: physical memory mapped by the process, including pages it "
                                  "shares with others (so the column does not add up to system memory used).");
        case IoRead:
        case IoWrite:
            return QStringLiteral("Bytes the process caused to be read from or written to storage, per second "
                                  "(/proc/PID/io). Only readable for your own processes; others show \u2014.");
        case Age: return QStringLiteral("Time since the process started");
        case Nice: return QStringLiteral("Scheduling niceness, -20 (highest priority) to 19");
        default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole) {
        switch (section) {
        case Name:
        case User:
        case State:
        case Command: return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
        default: return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------

ProcessFilterModel::ProcessFilterModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setSortRole(ProcessModel::SortRole);
    setDynamicSortFilter(true);
}

void ProcessFilterModel::setFilterString(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed == m_filter)
        return;
    changeRowsFilter([&] { m_filter = trimmed; });
}

void ProcessFilterModel::setShowKernelThreads(bool show)
{
    if (show == m_showKernel)
        return;
    changeRowsFilter([&] { m_showKernel = show; });
}

bool ProcessFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    const QVariant a = left.data(ProcessModel::SortRole);
    const QVariant b = right.data(ProcessModel::SortRole);
    if (a.typeId() == QMetaType::Double && b.typeId() == QMetaType::Double) {
        const double x = a.toDouble();
        const double y = b.toDouble();
        const bool xv = std::isfinite(x), yv = std::isfinite(y);
        if (xv != yv)
            return !xv; // unavailable below everything
        if (xv && x != y)
            return x < y;
    } else {
        const int c = QString::compare(a.toString(), b.toString(), Qt::CaseInsensitive);
        if (c != 0)
            return c < 0;
    }
    // Stable tie-break on PID so equal rows do not shuffle between refreshes.
    const QModelIndex lp = left.sibling(left.row(), ProcessModel::Pid);
    const QModelIndex rp = right.sibling(right.row(), ProcessModel::Pid);
    return lp.data(ProcessModel::SortRole).toDouble() < rp.data(ProcessModel::SortRole).toDouble();
}

bool ProcessFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!m_showKernel && idx.data(ProcessModel::KernelThreadRole).toBool())
        return false;
    if (m_filter.isEmpty())
        return true;
    return idx.data(ProcessModel::FilterTextRole).toString().contains(m_filter, Qt::CaseInsensitive);
}
