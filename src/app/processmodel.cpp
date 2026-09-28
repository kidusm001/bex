#include "app/processmodel.h"

#include "core/format.h"

#include <QFont>
#include <QString>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_map>
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
    : QAbstractItemModel(parent)
{
}

QModelIndex ProcessModel::index(int row, int column, const QModelIndex& parent) const
{
    if (column < 0 || column >= ColumnCount || row < 0)
        return {};
    if (!parent.isValid())
        return row < GroupCount ? createIndex(row, column, kGroupRow) : QModelIndex();
    if (parent.internalId() != kGroupRow || parent.column() != 0 || parent.row() < 0 || parent.row() >= GroupCount)
        return {};
    if (row >= groupSize(static_cast<Group>(parent.row())))
        return {};
    return createIndex(row, column, static_cast<quintptr>(parent.row()));
}

QModelIndex ProcessModel::parent(const QModelIndex& child) const
{
    if (!child.isValid() || child.internalId() == kGroupRow)
        return {};
    return createIndex(static_cast<int>(child.internalId()), 0, kGroupRow);
}

int ProcessModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return GroupCount;
    if (parent.internalId() == kGroupRow && parent.column() == 0)
        return groupSize(static_cast<Group>(parent.row()));
    return 0;
}

int ProcessModel::columnCount(const QModelIndex&) const
{
    return ColumnCount;
}

bool ProcessModel::hasChildren(const QModelIndex& parent) const
{
    return rowCount(parent) > 0;
}

Qt::ItemFlags ProcessModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    if (index.internalId() == kGroupRow)
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;
}

QModelIndex ProcessModel::groupIndex(Group group, int column) const
{
    return index(static_cast<int>(group), column);
}

int ProcessModel::processCount() const
{
    int n = 0;
    for (const auto& g : m_groups)
        n += static_cast<int>(g.size());
    return n;
}

const ps::ProcessInfo* ProcessModel::process(const QModelIndex& index) const
{
    if (!index.isValid() || index.model() != this || index.internalId() == kGroupRow)
        return nullptr;
    const auto& list = rows(static_cast<int>(index.internalId()));
    if (index.row() < 0 || index.row() >= static_cast<int>(list.size()))
        return nullptr;
    return &list[static_cast<std::size_t>(index.row())];
}

ProcessModel::Group ProcessModel::groupFor(const ps::ProcessInfo& p, const ProcessGrouping& grouping)
{
    if (p.kernelThread)
        return Kernel;
    if (grouping.state == ProcessGrouping::State::Detected && grouping.apps.count(keyOf(p)))
        return Apps;
    return Background;
}

QString ProcessModel::groupTitle(Group group, const ProcessGrouping& grouping)
{
    const bool detected = grouping.state == ProcessGrouping::State::Detected;
    switch (group) {
    case Apps:
        if (detected)
            return QStringLiteral("Apps");
        return grouping.state == ProcessGrouping::State::Pending ? QStringLiteral("Apps (detecting\u2026)")
                                                                 : QStringLiteral("App detection unavailable");
    case Background: return detected ? QStringLiteral("Background processes") : QStringLiteral("Unclassified user processes");
    case Kernel: return QStringLiteral("Kernel processes");
    case GroupCount: break;
    }
    return {};
}

bool ProcessModel::groupCounted(Group group, const ProcessGrouping& grouping)
{
    return group != Apps || grouping.state == ProcessGrouping::State::Detected;
}

QString ProcessModel::groupToolTip(Group group, const ProcessGrouping& grouping)
{
    const bool detected = grouping.state == ProcessGrouping::State::Detected;
    switch (group) {
    case Apps:
        if (detected)
            return QStringLiteral("Processes that own a window (from %1), including minimized, hidden and other-workspace "
                                  "windows. A process with several windows is listed once; helper processes without "
                                  "a window of their own are background processes.")
                .arg(grouping.source.isEmpty() ? QStringLiteral("the desktop") : grouping.source);
        if (grouping.state == ProcessGrouping::State::Pending)
            return QStringLiteral("Reading the desktop's window list. Until it arrives, user processes are not "
                                  "classified.");
        return (grouping.reason.isEmpty() ? QStringLiteral("The desktop's window list is not available.")
                                          : grouping.reason + QLatin1Char('.'))
            + QStringLiteral(" User processes are listed without deciding which are apps.");
    case Background:
        if (detected)
            return QStringLiteral("User-space processes of every user that own no window: services, daemons and helper "
                                  "processes. Only shown when the desktop identified the owner of every window.");
        return QStringLiteral("All user-space processes of every user. Which of them own a window is not known.");
    case Kernel:
        return QStringLiteral("Kernel threads (PF_KTHREAD). They have no command line, memory or disk I/O accounting of "
                              "their own.");
    case GroupCount: break;
    }
    return {};
}

void ProcessModel::clear()
{
    apply(nullptr);
}

void ProcessModel::setProcesses(const std::vector<ps::ProcessInfo>& processes)
{
    apply(&processes);
}

void ProcessModel::setGrouping(ProcessGrouping grouping)
{
    if (grouping == m_grouping)
        return;
    m_grouping = std::move(grouping);
    std::vector<ps::ProcessInfo> current;
    current.reserve(static_cast<std::size_t>(processCount()));
    for (const auto& g : m_groups)
        current.insert(current.end(), g.begin(), g.end());
    apply(&current);
}

void ProcessModel::apply(const std::vector<ps::ProcessInfo>* processes)
{
    struct Target {
        std::size_t index;
        int group;
    };
    std::unordered_map<ProcessKey, Target, ProcessKeyHash> incoming;
    if (processes) {
        incoming.reserve(processes->size());
        for (std::size_t i = 0; i < processes->size(); ++i) {
            const auto& p = (*processes)[i];
            incoming.emplace(keyOf(p), Target {i, groupFor(p, m_grouping)});
        }
    }

    // Remove vanished rows, merging contiguous runs, from the end so earlier
    // row numbers stay valid.
    for (int g = 0; g < GroupCount; ++g) {
        auto& list = rows(g);
        int row = static_cast<int>(list.size()) - 1;
        while (row >= 0) {
            if (incoming.count(keyOf(list[static_cast<std::size_t>(row)]))) {
                --row;
                continue;
            }
            int first = row;
            while (first > 0 && !incoming.count(keyOf(list[static_cast<std::size_t>(first - 1)])))
                --first;
            beginRemoveRows(groupIndex(static_cast<Group>(g)), first, row);
            list.erase(list.begin() + first, list.begin() + row + 1);
            endRemoveRows();
            row = first - 1;
        }
    }

    // Move rows whose group changed, a contiguous run with one destination
    // at a time, to the end of the destination group.
    for (int g = 0; g < GroupCount; ++g) {
        auto& list = rows(g);
        int row = static_cast<int>(list.size()) - 1;
        while (row >= 0) {
            const int to = incoming.at(keyOf(list[static_cast<std::size_t>(row)])).group;
            if (to == g) {
                --row;
                continue;
            }
            int first = row;
            while (first > 0 && incoming.at(keyOf(list[static_cast<std::size_t>(first - 1)])).group == to)
                --first;
            auto& dest = rows(to);
            const bool ok = beginMoveRows(groupIndex(static_cast<Group>(g)), first, row,
                                          groupIndex(static_cast<Group>(to)), static_cast<int>(dest.size()));
            Q_ASSERT(ok);
            Q_UNUSED(ok);
            dest.insert(dest.end(), std::make_move_iterator(list.begin() + first),
                        std::make_move_iterator(list.begin() + row + 1));
            list.erase(list.begin() + first, list.begin() + row + 1);
            endMoveRows();
            row = first - 1;
        }
    }

    // Update survivors in place.
    std::unordered_set<ProcessKey, ProcessKeyHash> present;
    present.reserve(incoming.size());
    for (int g = 0; g < GroupCount; ++g) {
        auto& list = rows(g);
        for (auto& existing : list) {
            const ProcessKey k = keyOf(existing);
            existing = (*processes)[incoming.at(k).index];
            present.insert(k);
        }
        if (!list.empty()) {
            const QModelIndex parent = groupIndex(static_cast<Group>(g));
            Q_EMIT dataChanged(index(0, 0, parent), index(static_cast<int>(list.size()) - 1, ColumnCount - 1, parent));
        }
    }

    // Append new processes to their groups.
    if (processes) {
        std::array<std::vector<std::size_t>, GroupCount> added;
        for (std::size_t i = 0; i < processes->size(); ++i) {
            const ProcessKey k = keyOf((*processes)[i]);
            if (present.insert(k).second) // also skips a duplicated identity
                added[static_cast<std::size_t>(incoming.at(k).group)].push_back(i);
        }
        for (int g = 0; g < GroupCount; ++g) {
            const auto& indices = added[static_cast<std::size_t>(g)];
            if (indices.empty())
                continue;
            auto& list = rows(g);
            const int start = static_cast<int>(list.size());
            beginInsertRows(groupIndex(static_cast<Group>(g)), start, start + static_cast<int>(indices.size()) - 1);
            for (const std::size_t i : indices)
                list.push_back((*processes)[i]);
            endInsertRows();
        }
    }
    emitGroupRowsChanged();
}

void ProcessModel::emitGroupRowsChanged()
{
    Q_EMIT dataChanged(index(0, 0), index(GroupCount - 1, 0));
}

QVariant ProcessModel::groupData(int group, int column, int role) const
{
    const auto g = static_cast<Group>(group);
    switch (role) {
    case IsGroupRole: return true;
    case GroupRole: return group;
    case SortRole: return static_cast<double>(group);
    case GroupTitleRole: return groupTitle(g, m_grouping);
    case GroupSizeRole: return groupSize(g);
    case CountedRole: return groupCounted(g, m_grouping);
    default: break;
    }
    if (column != Name)
        return {};
    switch (role) {
    case Qt::DisplayRole:
        return groupCounted(g, m_grouping) ? QStringLiteral("%1 (%2)").arg(groupTitle(g, m_grouping)).arg(groupSize(g))
                                           : groupTitle(g, m_grouping);
    case Qt::AccessibleTextRole:
        return groupCounted(g, m_grouping)
            ? QStringLiteral("%1, group of %2 processes").arg(groupTitle(g, m_grouping)).arg(groupSize(g))
            : QStringLiteral("%1, group").arg(groupTitle(g, m_grouping));
    case Qt::ToolTipRole: return groupToolTip(g, m_grouping);
    case Qt::FontRole: {
        QFont f; // only the weight is set, the view's font supplies the rest
        f.setBold(true);
        return f;
    }
    default: return {};
    }
}

QVariant ProcessModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.model() != this)
        return {};
    if (index.internalId() == kGroupRow)
        return groupData(index.row(), index.column(), role);
    const ps::ProcessInfo* found = process(index);
    if (!found)
        return {};
    const auto& p = *found;
    const int col = index.column();

    switch (role) {
    case KernelThreadRole: return p.kernelThread;
    case IsGroupRole: return false;
    case GroupRole: return static_cast<int>(index.internalId());
    case PidRole: return p.pid;
    case StartTicksRole: return static_cast<qulonglong>(p.startTicks);
    default: break;
    }
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
    for (int g = 0; g < GroupCount; ++g) {
        const int n = groupSize(static_cast<Group>(g));
        if (n == 0)
            continue;
        const QModelIndex parent = groupIndex(static_cast<Group>(g));
        Q_EMIT dataChanged(index(0, 0, parent), index(n - 1, ColumnCount - 1, parent), {Qt::DisplayRole, Qt::ToolTipRole});
    }
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
    // Group titles count the matches.
    if (rowCount() > 0)
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DisplayRole, Qt::AccessibleTextRole});
}

QModelIndex ProcessFilterModel::groupIndex(ProcessModel::Group group) const
{
    const auto* source = qobject_cast<const ProcessModel*>(sourceModel());
    return source ? mapFromSource(source->groupIndex(group)) : QModelIndex();
}

int ProcessFilterModel::visibleInGroup(ProcessModel::Group group) const
{
    const QModelIndex g = groupIndex(group);
    return g.isValid() ? rowCount(g) : 0;
}

int ProcessFilterModel::visibleProcessCount() const
{
    int n = 0;
    for (int g = 0; g < ProcessModel::GroupCount; ++g)
        n += visibleInGroup(static_cast<ProcessModel::Group>(g));
    return n;
}

QVariant ProcessFilterModel::data(const QModelIndex& index, int role) const
{
    if (isFiltering() && index.isValid() && !index.parent().isValid() && index.column() == ProcessModel::Name
        && (role == Qt::DisplayRole || role == Qt::AccessibleTextRole)
        && index.data(ProcessModel::CountedRole).toBool()) {
        const QString title = index.data(ProcessModel::GroupTitleRole).toString();
        const int total = index.data(ProcessModel::GroupSizeRole).toInt();
        const int shown = rowCount(index);
        if (role == Qt::DisplayRole)
            return QStringLiteral("%1 (%2 of %3)").arg(title).arg(shown).arg(total);
        return QStringLiteral("%1, group, %2 of %3 processes match the filter").arg(title).arg(shown).arg(total);
    }
    return QSortFilterProxyModel::data(index, role);
}

bool ProcessFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    if (!left.parent().isValid()) {
        // Groups keep their order whichever way the processes are sorted.
        const int a = left.data(ProcessModel::GroupRole).toInt();
        const int b = right.data(ProcessModel::GroupRole).toInt();
        return sortOrder() == Qt::AscendingOrder ? a < b : a > b;
    }
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
    return left.data(ProcessModel::PidRole).toInt() < right.data(ProcessModel::PidRole).toInt();
}

bool ProcessFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (!sourceParent.isValid() || m_filter.isEmpty())
        return true; // group rows always stay
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    return idx.data(ProcessModel::FilterTextRole).toString().contains(m_filter, Qt::CaseInsensitive);
}
