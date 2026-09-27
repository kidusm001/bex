#include "core/linuxparsers.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace ps::parse {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

bool startsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Parses the value part of a "Key:   123 kB" line, returning bytes. The kernel
// prints every field PulseScope reads with the " kB" unit, so a value without
// it (truncated or corrupted input) is rejected instead of read as bytes.
std::optional<uint64_t> kbValue(std::string_view rest)
{
    const auto tokens = splitWhitespace(rest);
    if (tokens.size() != 2 || tokens[1] != "kB")
        return std::nullopt;
    const auto v = toU64(tokens[0]);
    if (!v || *v > UINT64_MAX / 1024)
        return std::nullopt;
    return *v * 1024;
}

} // namespace

std::optional<uint64_t> toU64(std::string_view token)
{
    if (token.empty() || token.front() == '-' || token.front() == '+')
        return std::nullopt;
    uint64_t value = 0;
    const auto* end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(token.data(), end, value, 10);
    if (ec != std::errc() || ptr != end)
        return std::nullopt;
    return value;
}

std::optional<int64_t> toI64(std::string_view token)
{
    if (token.empty() || token.front() == '+')
        return std::nullopt;
    int64_t value = 0;
    const auto* end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(token.data(), end, value, 10);
    if (ec != std::errc() || ptr != end)
        return std::nullopt;
    return value;
}

std::optional<double> toDouble(std::string_view token)
{
    if (token.empty() || token.front() == '+')
        return std::nullopt;
    double value = 0;
    const auto* end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(token.data(), end, value, std::chars_format::fixed);
    if (ec != std::errc() || ptr != end || !std::isfinite(value))
        return std::nullopt;
    return value;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> splitWhitespace(std::string_view line)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isSpace(line[i]))
            ++i;
        const size_t start = i;
        while (i < line.size() && !isSpace(line[i]))
            ++i;
        if (i > start)
            out.push_back(line.substr(start, i - start));
    }
    return out;
}

std::vector<std::string_view> splitLines(std::string_view text)
{
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos)
            nl = text.size();
        out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::string sanitize(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const auto uc = static_cast<unsigned char>(c);
        out.push_back((uc < 0x20 || uc == 0x7f) ? '?' : c);
    }
    return out;
}

namespace {

// Length of the well-formed UTF-8 sequence (RFC 3629: no overlongs, no
// surrogates, nothing above U+10FFFF) starting at s[i], or 0 if there is none.
std::size_t utf8SequenceLength(std::string_view s, std::size_t i)
{
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const auto cont = [&](std::size_t k) { return k < s.size() && (b(k) & 0xc0) == 0x80; };
    const unsigned char c = b(i);
    if (c < 0x80)
        return 1;
    if (c >= 0xc2 && c <= 0xdf)
        return cont(i + 1) ? 2 : 0;
    if (c >= 0xe0 && c <= 0xef) {
        if (!cont(i + 1) || !cont(i + 2))
            return 0;
        if ((c == 0xe0 && b(i + 1) < 0xa0) || (c == 0xed && b(i + 1) > 0x9f))
            return 0;
        return 3;
    }
    if (c >= 0xf0 && c <= 0xf4) {
        if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3))
            return 0;
        if ((c == 0xf0 && b(i + 1) < 0x90) || (c == 0xf4 && b(i + 1) > 0x8f))
            return 0;
        return 4;
    }
    return 0;
}

} // namespace

std::string escapeName(std::string_view s)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto uc = static_cast<unsigned char>(s[i]);
        const std::size_t len = utf8SequenceLength(s, i);
        const bool c1Control = len == 2 && uc == 0xc2 && static_cast<unsigned char>(s[i + 1]) < 0xa0;
        if (s[i] == '\\') {
            out += "\\\\";
            ++i;
        } else if (len == 0 || uc < 0x20 || uc == 0x7f || c1Control) {
            // Control characters and bytes that are not well-formed UTF-8 are
            // escaped, so the result is printable and survives a round trip
            // through QString unchanged (keys stay distinct in the GUI).
            const std::size_t n = len == 0 ? 1 : len;
            for (std::size_t k = 0; k < n; ++k) {
                const auto byte = static_cast<unsigned char>(s[i + k]);
                out += "\\x";
                out.push_back(kHex[byte >> 4]);
                out.push_back(kHex[byte & 0xf]);
            }
            i += n;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------

ProcStat parseProcStat(std::string_view text)
{
    ProcStat result;
    for (auto line : splitLines(text)) {
        if (startsWith(line, "cpu")) {
            const auto tokens = splitWhitespace(line);
            if (tokens.size() < 5) // name + user nice system idle at minimum
                continue;
            int index = -1;
            const auto name = tokens[0];
            if (name != "cpu") {
                const auto idx = toU64(name.substr(3));
                if (!idx || *idx > 1u << 20)
                    continue;
                index = static_cast<int>(*idx);
            }
            uint64_t fields[10] = {};
            bool ok = true;
            const size_t count = std::min<size_t>(tokens.size() - 1, 10);
            for (size_t i = 0; i < count; ++i) {
                const auto v = toU64(tokens[i + 1]);
                if (!v) {
                    ok = false;
                    break;
                }
                fields[i] = *v;
            }
            if (!ok)
                continue;
            CpuTimes t;
            t.user = fields[0];
            t.nice = fields[1];
            t.system = fields[2];
            t.idle = fields[3];
            t.iowait = fields[4];
            t.irq = fields[5];
            t.softirq = fields[6];
            t.steal = fields[7];
            t.guest = fields[8];
            t.guestNice = fields[9];
            t.fields = static_cast<int>(count);
            if (index < 0) {
                result.aggregate = t;
            } else {
                auto dup = std::find_if(result.cpus.begin(), result.cpus.end(),
                                        [index](const CpuLine& c) { return c.index == index; });
                if (dup == result.cpus.end())
                    result.cpus.push_back({index, t});
            }
        } else if (startsWith(line, "btime")) {
            const auto tokens = splitWhitespace(line);
            if (tokens.size() == 2 && tokens[0] == "btime")
                result.bootTime = toU64(tokens[1]);
        }
    }
    std::sort(result.cpus.begin(), result.cpus.end(),
              [](const CpuLine& a, const CpuLine& b) { return a.index < b.index; });
    return result;
}

MemInfoRaw parseMeminfo(std::string_view text)
{
    MemInfoRaw m;
    for (auto line : splitLines(text)) {
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        const auto key = line.substr(0, colon);
        const auto value = kbValue(line.substr(colon + 1));
        if (!value)
            continue;
        if (key == "MemTotal") m.memTotal = value;
        else if (key == "MemFree") m.memFree = value;
        else if (key == "MemAvailable") m.memAvailable = value;
        else if (key == "Buffers") m.buffers = value;
        else if (key == "Cached") m.cached = value;
        else if (key == "SReclaimable") m.sReclaimable = value;
        else if (key == "Shmem") m.shmem = value;
        else if (key == "SwapTotal") m.swapTotal = value;
        else if (key == "SwapFree") m.swapFree = value;
    }
    return m;
}

std::vector<DiskStatLine> parseDiskstats(std::string_view text)
{
    std::vector<DiskStatLine> out;
    for (auto line : splitLines(text)) {
        const auto t = splitWhitespace(line);
        // major minor name + at least 11 statistics fields
        if (t.size() < 14)
            continue;
        const auto major = toU64(t[0]);
        const auto minor = toU64(t[1]);
        const auto sectorsRead = toU64(t[5]);
        const auto sectorsWritten = toU64(t[9]);
        const auto ioTicks = toU64(t[12]);
        if (!major || !minor || !sectorsRead || !sectorsWritten || !ioTicks)
            continue;
        if (*major > UINT32_MAX || *minor > UINT32_MAX)
            continue;
        DiskStatLine d;
        d.major = static_cast<unsigned>(*major);
        d.minor = static_cast<unsigned>(*minor);
        d.name = std::string(t[2]);
        d.sectorsRead = *sectorsRead;
        d.sectorsWritten = *sectorsWritten;
        d.ioTicksMs = *ioTicks;
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<NetDevLine> parseNetDev(std::string_view text)
{
    std::vector<NetDevLine> out;
    for (auto line : splitLines(text)) {
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        // The header lines have no colon. Linux interface names may contain
        // any byte except '/', ':' and whitespace (dev_valid_name), so '|' is
        // a legal character and the colon plus numeric fields identify rows.
        const auto name = trim(line.substr(0, colon));
        if (name.empty() || name.find_first_of(" \t/") != std::string_view::npos)
            continue;
        const auto t = splitWhitespace(line.substr(colon + 1));
        if (t.size() < 16)
            continue;
        const auto rx = toU64(t[0]);
        const auto tx = toU64(t[8]);
        if (!rx || !tx)
            continue;
        out.push_back({std::string(name), *rx, *tx});
    }
    return out;
}

std::optional<PidStat> parsePidStat(std::string_view text)
{
    // "pid (comm) state ppid ..." where comm may contain spaces and parentheses;
    // the last ')' always terminates it.
    const auto open = text.find('(');
    const auto close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return std::nullopt;
    const auto pid = toI64(trim(text.substr(0, open)));
    if (!pid || *pid <= 0 || *pid > INT32_MAX)
        return std::nullopt;
    const auto rest = splitWhitespace(text.substr(close + 1));
    // rest[0] is field 3 (state); we need up to field 24 (rss) => 22 entries.
    if (rest.size() < 22 || rest[0].size() != 1)
        return std::nullopt;
    PidStat s;
    s.pid = static_cast<int>(*pid);
    s.comm = sanitize(text.substr(open + 1, close - open - 1));
    s.state = rest[0][0];
    const auto ppid = toI64(rest[1]);
    const auto flags = toU64(rest[6]);
    const auto utime = toU64(rest[11]);
    const auto stime = toU64(rest[12]);
    const auto nice = toI64(rest[16]);
    const auto threads = toI64(rest[17]);
    const auto start = toU64(rest[19]);
    const auto rss = toI64(rest[21]);
    if (!ppid || !flags || !utime || !stime || !nice || !threads || !start || !rss)
        return std::nullopt;
    if (*ppid < 0 || *ppid > INT32_MAX || *flags > UINT32_MAX || *threads < 0 || *rss < 0)
        return std::nullopt;
    s.ppid = static_cast<int>(*ppid);
    s.flags = static_cast<unsigned>(*flags);
    s.utime = *utime;
    s.stime = *stime;
    s.nice = *nice;
    s.numThreads = *threads;
    s.startTime = *start;
    s.rssPages = *rss;
    return s;
}

std::optional<PidIo> parsePidIo(std::string_view text)
{
    std::optional<uint64_t> readBytes, writeBytes;
    for (auto line : splitLines(text)) {
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        const auto key = line.substr(0, colon);
        const auto value = toU64(trim(line.substr(colon + 1)));
        if (key == "read_bytes")
            readBytes = value;
        else if (key == "write_bytes")
            writeBytes = value;
    }
    if (!readBytes || !writeBytes)
        return std::nullopt;
    return PidIo{*readBytes, *writeBytes};
}

std::optional<uint32_t> parseStatusRealUid(std::string_view text)
{
    for (auto line : splitLines(text)) {
        if (!startsWith(line, "Uid:"))
            continue;
        const auto t = splitWhitespace(line.substr(4));
        if (t.empty())
            return std::nullopt;
        const auto uid = toU64(t[0]);
        if (!uid || *uid > UINT32_MAX)
            return std::nullopt;
        return static_cast<uint32_t>(*uid);
    }
    return std::nullopt;
}

std::string parseCmdline(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char c : raw)
        out.push_back(c == '\0' ? ' ' : c);
    return sanitize(trim(out));
}

// ---------------------------------------------------------------------------

bool MountEntry::readOnly() const
{
    size_t start = 0;
    while (start <= options.size()) {
        size_t comma = options.find(',', start);
        if (comma == std::string::npos)
            comma = options.size();
        if (std::string_view(options).substr(start, comma - start) == "ro")
            return true;
        start = comma + 1;
    }
    return false;
}

std::string unescapeMountField(std::string_view field)
{
    std::string out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            const auto a = field[i + 1], b = field[i + 2], c = field[i + 3];
            if (a >= '0' && a <= '3' && b >= '0' && b <= '7' && c >= '0' && c <= '7') {
                const int decoded = (a - '0') * 64 + (b - '0') * 8 + (c - '0');
                // Paths cannot contain NUL; decoding "\000" would make c_str()
                // name a different, shorter path. Keep such a sequence literal.
                if (decoded != 0) {
                    out.push_back(static_cast<char>(decoded));
                    i += 3;
                    continue;
                }
            }
        }
        out.push_back(field[i]);
    }
    return out;
}

std::vector<MountEntry> parseMounts(std::string_view text)
{
    std::vector<MountEntry> out;
    for (auto line : splitLines(text)) {
        const auto t = splitWhitespace(line);
        if (t.size() < 4)
            continue;
        MountEntry e;
        e.source = unescapeMountField(t[0]);
        e.mountPoint = unescapeMountField(t[1]);
        e.fsType = std::string(t[2]);
        e.options = std::string(t[3]);
        if (e.mountPoint.empty() || e.mountPoint.front() != '/')
            continue;
        out.push_back(std::move(e));
    }
    return out;
}

bool isLocalCapacityFs(const MountEntry& e)
{
    static constexpr std::string_view kBlockTypes[] = {
        "ext2", "ext3", "ext4", "xfs", "btrfs", "f2fs", "vfat", "msdos", "exfat",
        "ntfs", "ntfs3", "jfs", "reiserfs", "nilfs2", "hfs", "hfsplus", "bcachefs",
        "udf",
    };
    const std::string_view type = e.fsType;
    if (type == "zfs") // datasets are named pool/dataset rather than /dev paths
        return !e.source.empty() && e.source.front() != '/';
    for (auto t : kBlockTypes) {
        if (type == t)
            return startsWith(e.source, "/dev/");
    }
    return false; // network, FUSE, autofs, pseudo and memory filesystems
}

std::vector<MountEntry> parseMountInfo(std::string_view text)
{
    // "36 35 98:0 /root /mnt rw,noatime shared:1 - ext4 /dev/sda1 rw,errors=continue"
    std::vector<MountEntry> out;
    for (auto line : splitLines(text)) {
        const auto t = splitWhitespace(line);
        if (t.size() < 10)
            continue;
        std::size_t sep = 6;
        while (sep < t.size() && t[sep] != "-")
            ++sep;
        if (sep + 2 >= t.size())
            continue;
        const auto id = toU64(t[0]);
        const auto parent = toU64(t[1]);
        const auto colon = t[2].find(':');
        if (!id || !parent || *id > INT32_MAX || *parent > INT32_MAX || colon == std::string_view::npos)
            continue;
        const auto major = toU64(t[2].substr(0, colon));
        const auto minor = toU64(t[2].substr(colon + 1));
        if (!major || !minor || *major > UINT32_MAX || *minor > UINT32_MAX)
            continue;
        MountEntry e;
        e.id = static_cast<int>(*id);
        e.parentId = static_cast<int>(*parent);
        e.major = static_cast<unsigned>(*major);
        e.minor = static_cast<unsigned>(*minor);
        e.hasDevice = true;
        e.root = unescapeMountField(t[3]);
        e.mountPoint = unescapeMountField(t[4]);
        e.options = std::string(t[5]);
        e.fsType = std::string(t[sep + 1]);
        e.source = unescapeMountField(t[sep + 2]);
        if (e.mountPoint.empty() || e.mountPoint.front() != '/')
            continue;
        out.push_back(std::move(e));
    }
    return out;
}

bool isLookupSafeFs(std::string_view type)
{
    static constexpr std::string_view kSafe[] = {
        "ext2", "ext3", "ext4", "xfs", "btrfs", "f2fs", "vfat", "msdos", "exfat", "ntfs", "ntfs3", "jfs",
        "reiserfs", "nilfs2", "hfs", "hfsplus", "bcachefs", "udf", "zfs", "iso9660", "squashfs", "erofs",
        "rootfs", "tmpfs", "ramfs", "devtmpfs", "proc", "sysfs", "cgroup", "cgroup2", "devpts", "mqueue",
        "hugetlbfs", "securityfs", "debugfs", "tracefs", "pstore", "efivarfs", "bpf", "configfs",
        "binfmt_misc", "fusectl", "nsfs", "overlay",
    };
    return std::find(std::begin(kSafe), std::end(kSafe), type) != std::end(kSafe);
}

MountResolution resolveMountPath(const std::vector<MountEntry>& table, std::string_view path)
{
    MountResolution r;
    if (table.empty() || path.empty() || path.front() != '/')
        return r;
    const int n = static_cast<int>(table.size());
    auto findById = [&](int id) {
        for (int i = 0; i < n; ++i)
            if (table[i].id == id)
                return i;
        return -1;
    };
    bool safe = true;
    // Follows mounts stacked on the same directory: the last one mounted on
    // top of `cur` at the same point is the one path lookups see.
    auto topOf = [&](int cur, std::string_view mp) {
        for (int guard = 0; guard < n; ++guard) {
            int next = -1;
            for (int i = 0; i < n; ++i)
                if (i != cur && table[i].parentId == table[cur].id && table[i].mountPoint == mp)
                    next = i;
            if (next < 0)
                break;
            if (!isLookupSafeFs(table[cur].fsType))
                safe = false; // crossing an autofs/FUSE trigger point
            cur = next;
        }
        return cur;
    };
    int cur = -1;
    for (int i = 0; i < n && cur < 0; ++i) {
        const auto& e = table[i];
        if (e.mountPoint == "/" && (e.parentId == e.id || findById(e.parentId) < 0))
            cur = i;
    }
    if (cur < 0)
        return r;
    cur = topOf(cur, "/");
    std::size_t pos = 1;
    while (pos <= path.size()) {
        std::size_t next = path.find('/', pos);
        if (next == std::string_view::npos)
            next = path.size();
        if (next > pos) {
            const auto prefix = path.substr(0, next);
            if (!isLookupSafeFs(table[cur].fsType))
                safe = false; // the component is looked up inside `cur`
            int child = -1;
            for (int i = 0; i < n; ++i)
                if (table[i].parentId == table[cur].id && table[i].mountPoint == prefix && i != cur)
                    child = child < 0 ? i : child;
            if (child >= 0)
                cur = topOf(child, prefix);
        }
        pos = next + 1;
    }
    r.index = cur;
    r.safe = safe;
    return r;
}

// ---------------------------------------------------------------------------

std::optional<double> parseMilliCelsius(std::string_view text)
{
    const auto v = toI64(trim(text));
    if (!v)
        return std::nullopt;
    const double c = static_cast<double>(*v) / 1000.0;
    // Below -60 C or above 200 C is not a plausible reading for hardware that
    // is running this program; drivers use such values as error markers.
    if (c < -60.0 || c > 200.0)
        return std::nullopt;
    return c;
}

std::optional<LoadAvg> parseLoadavg(std::string_view text)
{
    const auto t = splitWhitespace(text);
    if (t.size() < 3)
        return std::nullopt;
    const auto a = toDouble(t[0]), b = toDouble(t[1]), c = toDouble(t[2]);
    if (!a || !b || !c || *a < 0 || *b < 0 || *c < 0)
        return std::nullopt;
    return LoadAvg{*a, *b, *c};
}

std::optional<double> parseUptime(std::string_view text)
{
    const auto t = splitWhitespace(text);
    if (t.empty())
        return std::nullopt;
    const auto v = toDouble(t[0]);
    if (!v || *v < 0)
        return std::nullopt;
    return v;
}

} // namespace ps::parse
