#include "app/settingsimport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QTemporaryFile>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace settingsimport {

namespace {

enum class Entry { Absent, Present, Unknown };

// Whether anything, a dangling symbolic link too, is at path. Unknown (with
// *err set) when that cannot be told, for example because a folder on the
// way cannot be searched.
Entry entryAt(const QString& path, int* err)
{
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) == 0)
        return Entry::Present;
    if (errno == ENOENT || errno == ENOTDIR)
        return Entry::Absent;
    *err = errno;
    return Entry::Unknown;
}

QString shown(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

Result failure(const QString& legacyPath, const QString& reason)
{
    return {Outcome::Failed, 0,
            QStringLiteral("Could not import the PulseScope settings from %1: %2. bex starts with its default "
                           "settings; %1 is unchanged.")
                .arg(shown(legacyPath), reason)};
}

// Moves from to to unless to exists; 0 or an errno value (EEXIST: it does).
// QFile::rename() is not used: when its rename fails it copies over the
// destination.
int renameNoReplace(const QString& from, const QString& to)
{
    const QByteArray src = QFile::encodeName(from);
    const QByteArray dst = QFile::encodeName(to);
    if (::renameat2(AT_FDCWD, src.constData(), AT_FDCWD, dst.constData(), RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS)
        return errno;
    // A filesystem without RENAME_NOREPLACE: a hard link is not made over
    // an existing file either.
    if (::link(src.constData(), dst.constData()) != 0)
        return errno;
    ::unlink(src.constData());
    return 0;
}

} // namespace

QString defaultFile()
{
    return QSettings().fileName();
}

QString legacyFile()
{
    // QSettings keeps <organization>/<application>.conf in the user's
    // configuration folder, and PulseScope used "PulseScope" for both. The
    // path is derived, not asked of a QSettings, which would read the file.
    const QString config = QFileInfo(QFileInfo(defaultFile()).absolutePath()).absolutePath();
    return QDir(config).filePath(QStringLiteral("PulseScope/PulseScope.conf"));
}

Result importLegacy(const QString& legacyPath, const QString& newPath)
{
    int newErr = 0;
    const Entry newEntry = entryAt(newPath, &newErr);
    if (newEntry == Entry::Present)
        return {Outcome::NewSettingsExist, 0, {}};

    const QByteArray legacyName = QFile::encodeName(legacyPath);
    struct stat st {};
    if (::stat(legacyName.constData(), &st) != 0) {
        const int err = errno;
        if (err != ENOENT && err != ENOTDIR)
            return failure(legacyPath, QStringLiteral("it cannot be checked (%1)").arg(qt_error_string(err)));
        int linkErr = 0;
        switch (entryAt(legacyPath, &linkErr)) {
        case Entry::Absent:
            return {};
        case Entry::Present:
            return failure(legacyPath, QStringLiteral("it is a symbolic link to a missing file"));
        case Entry::Unknown:
            return failure(legacyPath, QStringLiteral("it cannot be checked (%1)").arg(qt_error_string(linkErr)));
        }
    }
    if (!S_ISREG(st.st_mode))
        return failure(legacyPath, QStringLiteral("it is not a file"));
    if (newEntry == Entry::Unknown)
        return failure(legacyPath, QStringLiteral("whether bex's settings file %1 exists cannot be checked (%2)")
                                       .arg(shown(newPath), qt_error_string(newErr)));

    // Non-blocking, so that a file replaced by a FIFO meanwhile cannot hang
    // the start; checked again once open.
    const int fd = ::open(legacyName.constData(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return failure(legacyPath, QStringLiteral("the file cannot be read (%1)").arg(qt_error_string(errno)));
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer {fd};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return failure(legacyPath, QStringLiteral("it is not a file"));
    // At most one byte past the limit is read, whatever the file's size
    // says or becomes; a file over the limit fails rather than being
    // imported in part.
    QByteArray bytes;
    char buf[64 * 1024];
    while (bytes.size() <= kMaxLegacyBytes) {
        const auto want = static_cast<std::size_t>(std::min<qint64>(sizeof buf, kMaxLegacyBytes + 1 - bytes.size()));
        const ssize_t n = ::read(fd, buf, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return failure(legacyPath, QStringLiteral("the file cannot be read (%1)").arg(qt_error_string(errno)));
        }
        if (n == 0)
            break;
        bytes.append(buf, n);
    }
    if (bytes.size() > kMaxLegacyBytes)
        return failure(legacyPath, QStringLiteral("the file is larger than %1 MiB, the most bex imports")
                                       .arg(kMaxLegacyBytes >> 20));
    if (bytes.isEmpty())
        return {};

    // QSettings caches each file it has read for the whole process and, while
    // the file's size and time are unchanged, neither reads it again nor
    // reports its errors again. A private copy of the bytes just read is
    // parsed instead, so the result never depends on an earlier read.
    QTemporaryFile snapshot(QDir(QDir::tempPath()).filePath(QStringLiteral("bex-settings-import-XXXXXX.conf")));
    if (!snapshot.open() || snapshot.write(bytes) != bytes.size() || !snapshot.flush())
        return failure(legacyPath, QStringLiteral("it cannot be checked, as nothing can be written in %1 (%2)")
                                       .arg(shown(QDir::tempPath()), snapshot.errorString()));
    const QString snapshotPath = snapshot.fileName();
    snapshot.close();
    QSettings::SettingsMap values;
    QSettings::Status status = QSettings::NoError;
    {
        const QSettings legacy(snapshotPath, QSettings::NativeFormat);
        // Lines inside a section are parsed, and their errors reported, only
        // when first read: read everything before asking for the status.
        const QStringList keys = legacy.allKeys();
        for (const QString& key : keys) {
            // A "=value" line reads as an empty key, which QSettings can
            // neither return nor store.
            if (!key.isEmpty())
                values.insert(key, legacy.value(key));
        }
        status = legacy.status();
    }
    switch (status) {
    case QSettings::NoError:
        break;
    case QSettings::AccessError:
        return failure(legacyPath, QStringLiteral("the file cannot be read"));
    case QSettings::FormatError:
        return failure(legacyPath, QStringLiteral("the file is damaged (it is not a valid settings file)"));
    }
    if (values.isEmpty())
        return {};

    const QFileInfo newInfo(newPath);
    const QString dir = newInfo.absolutePath();
    if (!QDir().mkpath(dir))
        return failure(legacyPath, QStringLiteral("the folder %1 cannot be created").arg(shown(dir)));
    // Removed on every return unless renamed into place.
    QTemporaryFile temp(QDir(dir).filePath(QStringLiteral(".%1.import-XXXXXX").arg(newInfo.fileName())));
    if (!temp.open())
        return failure(legacyPath, QStringLiteral("nothing can be written in %1 (%2)").arg(shown(dir), temp.errorString()));
    const QString tempPath = temp.fileName();
    temp.close();
    {
        QSettings copy(tempPath, QSettings::NativeFormat);
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            copy.setValue(it.key(), it.value());
        copy.sync();
        if (copy.status() != QSettings::NoError)
            return failure(legacyPath, QStringLiteral("nothing can be written in %1").arg(shown(dir)));
    }
    if (const int err = renameNoReplace(tempPath, newPath); err != 0) {
        // Another bex started at the same moment and imported first, or the
        // user's own file appeared: it is kept.
        int probeErr = 0;
        if (err == EEXIST || entryAt(newPath, &probeErr) == Entry::Present)
            return {Outcome::NewSettingsExist, 0, {}};
        return failure(legacyPath, QStringLiteral("%1 cannot be created (%2)").arg(shown(newPath), qt_error_string(err)));
    }
    temp.setAutoRemove(false);

    const auto count = static_cast<int>(values.size());
    return {Outcome::Imported, count,
            QStringLiteral("Imported %1 %2 from PulseScope, this app's former name, from %3 into %4; %3 is unchanged.")
                .arg(QString::number(count), count == 1 ? QStringLiteral("setting") : QStringLiteral("settings"),
                     shown(legacyPath), shown(newPath))};
}

} // namespace settingsimport
