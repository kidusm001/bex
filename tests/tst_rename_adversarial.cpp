// Independent adversarial checks for the PulseScope -> bex rename.
//
// 1. settingsimport::importLegacy with explicit temporary paths: the legacy
//    settings are copied key by key (types intact) only when the new file
//    does not exist, the legacy file is never changed, and every failure is
//    Failed with a message and leaves no new or temporary file behind.
// 2. The built bex binary as a child process with a private HOME and XDG_*
//    tree (never the user's configuration): CLI exits write nothing; a start
//    imports once; existing bex settings win; later starts never re-import;
//    failures are reported on stderr.
// 3. With RN_STAGE (a private staged install tree), the installed names and
//    the desktop entry / AppStream / icon identity agree.
//
// This process itself runs with HOME and XDG_CONFIG_HOME pointed at a
// temporary directory before Qt starts, so nothing here can reach the
// user's real settings.

#include "app/settingsimport.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRect>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

using settingsimport::Outcome;

namespace {

QString g_privateRoot; // this test process's own HOME / XDG_CONFIG_HOME

QString appPath()
{
    for (const char* var : {"RN_APP", "BEX_APP", "PS_GU_APP"}) {
        const QString env = qEnvironmentVariable(var);
        if (!env.isEmpty())
            return env;
    }
    const QString sibling = QCoreApplication::applicationDirPath() + QStringLiteral("/bex");
    return QFileInfo(sibling).isExecutable() ? sibling : QString();
}

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

const char* outcomeName(Outcome o)
{
    switch (o) {
    case Outcome::Imported:
        return "Imported";
    case Outcome::NewSettingsExist:
        return "NewSettingsExist";
    case Outcome::NoLegacySettings:
        return "NoLegacySettings";
    case Outcome::Failed:
        return "Failed";
    }
    return "<invalid>";
}

QString describe(const settingsimport::Result& r)
{
    return QStringLiteral("%1, keys %2, \"%3\"").arg(QString::fromLatin1(outcomeName(r.outcome))).arg(r.keys).arg(r.message);
}

// Every entry under a directory (lstat: symlinks are not followed), with the
// contents of regular files, as a comparable snapshot.
std::map<QString, QByteArray> tree(const QString& root)
{
    std::map<QString, QByteArray> t;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QFileInfo fi(p);
        const QString rel = QDir(root).relativeFilePath(p);
        struct stat st {};
        const bool haveStat = ::lstat(QFile::encodeName(p).constData(), &st) == 0;
        const QByteArray mode = haveStat ? QByteArray::number(st.st_mode, 8) : QByteArray("?");
        if (fi.isSymLink())
            t[rel] = "<link " + QFile::encodeName(fi.symLinkTarget()) + ">";
        else if (fi.isDir())
            t[rel] = "<dir " + mode + ">";
        else
            t[rel] = mode + ' ' + QCryptographicHash::hash(fileBytes(p), QCryptographicHash::Sha256).toHex();
    }
    return t;
}

QString describeTree(const std::map<QString, QByteArray>& t)
{
    QStringList s;
    for (const auto& [k, v] : t)
        s << k + QLatin1Char('=') + QString::fromLatin1(v.left(20));
    return QLatin1Char('[') + s.join(QStringLiteral(", ")) + QLatin1Char(']');
}

// Values PulseScope 1.0 wrote, plus values it never wrote: an unknown key,
// a group, and types Qt serialises specially.
const QByteArray kBlob("\x00\x01\xfe\xff binary\n=[]", 15);
const QByteArray kGeometry("\x01\xd9\xd0\xcb\x00\x03\x00\x00\x00\x00\x00\x10", 12);
const QRect kRect(7, 8, 640, 480);
const QStringList kList {QStringLiteral("a,b"), QStringLiteral("ü ✓"), QString()};
const QString kFuture = QStringLiteral("kept = as is; \"quoted\" \\ #not a comment");
constexpr int kLegacyKeys = 11;
bool writeLegacyAt(const QString& path);

// Written through QSettings at a scratch path and then copied byte for
// byte: QSettings caches parsed files per absolute path in this process, so
// the code under test must never see a file this test parsed at that path.
bool writeLegacy(const QString& path)
{
    QTemporaryDir scratch;
    const QString src = scratch.filePath(QStringLiteral("legacy.conf"));
    if (!writeLegacyAt(src))
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    return writeBytes(path, fileBytes(src));
}

bool writeLegacyAt(const QString& path)
{
    QSettings s(path, QSettings::IniFormat);
    s.setValue(QStringLiteral("intervalMs"), 5000);
    s.setValue(QStringLiteral("windowSeconds"), 900);
    s.setValue(QStringLiteral("details"), true);
    s.setValue(QStringLiteral("smoothGraphs"), false);
    s.setValue(QStringLiteral("geometry"), kGeometry);
    s.setValue(QStringLiteral("processView/sortColumn"), 3);
    s.setValue(QStringLiteral("futureKey"), kFuture);
    s.setValue(QStringLiteral("Custom/blob"), kBlob);
    s.setValue(QStringLiteral("Custom/rect"), kRect);
    s.setValue(QStringLiteral("Custom/list"), kList);
    s.setValue(QStringLiteral("Custom/Deep/empty"), QString());
    s.sync();
    return s.status() == QSettings::NoError && s.allKeys().size() == kLegacyKeys;
}

// ---- black-box runs ----------------------------------------------------------

struct PrivateHome {
    QTemporaryDir dir;
    QString home() const { return dir.path(); }
    QString config() const { return dir.filePath(QStringLiteral("cfg")); }
    QString legacy() const { return config() + QStringLiteral("/PulseScope/PulseScope.conf"); }
    QString fresh() const { return config() + QStringLiteral("/bex/bex.conf"); }
    PrivateHome()
    {
        for (const char* d : {"cfg", "data", "cache", "run"})
            QDir(dir.path()).mkpath(QString::fromLatin1(d));
        ::chmod(QFile::encodeName(dir.filePath(QStringLiteral("run"))).constData(), 0700);
    }
};

struct Run {
    int exitCode = -1;
    bool finished = false;
    QString out, err;
};

Run runApp(const PrivateHome& h, const QStringList& args, int timeoutMs = 30000)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Nothing of the user's session: no display, compositor, bus, theme or
    // configuration can be reached from the child.
    for (const char* v : {"QT_QPA_PLATFORMTHEME", "QT_STYLE_OVERRIDE", "QT_QPA_PLATFORM", "QT_SCALE_FACTOR",
                          "QT_FONT_DPI", "DISPLAY", "WAYLAND_DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "SWAYSOCK",
                          "XDG_CONFIG_DIRS", "XDG_REAL_CONFIG_HOME", "DBUS_SESSION_BUS_ADDRESS", "XAUTHORITY",
                          "SESSION_MANAGER"})
        env.remove(QString::fromLatin1(v));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("HOME"), h.home());
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), h.config());
    env.insert(QStringLiteral("XDG_CONFIG_DIRS"), h.dir.filePath(QStringLiteral("nodirs")));
    env.insert(QStringLiteral("XDG_DATA_HOME"), h.dir.filePath(QStringLiteral("data")));
    env.insert(QStringLiteral("XDG_CACHE_HOME"), h.dir.filePath(QStringLiteral("cache")));
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), h.dir.filePath(QStringLiteral("run")));
    QProcess p;
    p.setProcessEnvironment(env);
    p.setWorkingDirectory(h.home());
    p.start(appPath(), args);
    Run r;
    if (!p.waitForStarted(5000))
        return r;
    if (!p.waitForFinished(timeoutMs)) {
        p.kill(); // this QProcess's own child only
        p.waitForFinished(5000);
        r.err = QString::fromLocal8Bit(p.readAllStandardError());
        return r;
    }
    r.finished = true;
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    r.out = QString::fromLocal8Bit(p.readAllStandardOutput());
    r.err = QString::fromLocal8Bit(p.readAllStandardError());
    return r;
}

Run startAndClose(const PrivateHome& h)
{
    return runApp(h, {QStringLiteral("--quit-after"), QStringLiteral("1")});
}

// The import report lines ("bex: ..." naming the legacy file); other
// "bex: " lines (a theme note) are not counted.
QStringList bexLines(const QString& err)
{
    QStringList lines;
    for (const QString& l : err.split(QLatin1Char('\n')))
        if (l.startsWith(QStringLiteral("bex: ")) && l.contains(QStringLiteral("PulseScope")))
            lines << l;
    return lines;
}

// Sets a directory's mode for one scope and restores the original on every
// return, before the QTemporaryDir that holds it (declared earlier) is
// removed.
struct ModeGuard {
    QByteArray path;
    mode_t restore = 0700;
    bool ok = false;
    ModeGuard(const QString& p, mode_t mode) : path(QFile::encodeName(p))
    {
        struct stat st {};
        if (::stat(path.constData(), &st) != 0)
            return;
        restore = st.st_mode & 07777;
        ok = ::chmod(path.constData(), mode) == 0;
    }
    ~ModeGuard()
    {
        if (ok)
            ::chmod(path.constData(), restore);
    }
    ModeGuard(const ModeGuard&) = delete;
    ModeGuard& operator=(const ModeGuard&) = delete;
};

bool sanitizerSilent(const QString& err)
{
    return !err.contains(QStringLiteral("AddressSanitizer")) && !err.contains(QStringLiteral("LeakSanitizer")) &&
           !err.contains(QStringLiteral("runtime error:"));
}

} // namespace

class TestRenameAdversarial : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // importLegacy, explicit paths
    void importCopiesEveryKeyWithTypes();
    void importNeverTouchesExistingNew_data();
    void importNeverTouchesExistingNew();
    void importWithoutLegacyWritesNothing_data();
    void importWithoutLegacyWritesNothing();
    void importFailuresLeaveNothing_data();
    void importFailuresLeaveNothing();
    void importIsNotRepeated();
    void importLegacyParentPermissions_data();
    void importReadCap_data();
    void importReadCap();
    void importLegacyParentPermissions();
    void defaultPathsFollowXdg();
    // the bex binary
    void cliExitsWriteNothing_data();
    void cliExitsWriteNothing();
    void versionHelpAndErrorsSayBex();
    void startupImportsOnceAndReports();
    void existingBexSettingsWin();
    void laterStartDoesNotReimport();
    void noLegacyStartsFresh();
    void blockedTargetIsReportedAndLegacyKept();
    void unreadableLegacyIsReportedNotImported();
    void unsearchableLegacyFolderIsReported();
    // installed identity
    void stagedInstallIdentity();
};

void TestRenameAdversarial::importCopiesEveryKeyWithTypes()
{
    QTemporaryDir d;
    const QString legacy = d.filePath(QStringLiteral("old/PulseScope.conf"));
    const QString fresh = d.filePath(QStringLiteral("new/sub/bex.conf")); // directories created as needed
    QVERIFY(writeLegacy(legacy));
    const QByteArray legacyBytes = fileBytes(legacy);
    struct stat before {};
    QCOMPARE(::stat(QFile::encodeName(legacy).constData(), &before), 0);

    const settingsimport::Result r = settingsimport::importLegacy(legacy, fresh);
    QVERIFY2(r.outcome == Outcome::Imported, qPrintable(describe(r)));
    QCOMPARE(r.keys, kLegacyKeys);
    QVERIFY2(r.message.contains(legacy) && r.message.contains(fresh), qPrintable(r.message));
    QVERIFY2(r.message.contains(QString::number(kLegacyKeys)), qPrintable(r.message));

    // The legacy file: same bytes, same inode and mtime (never rewritten).
    QCOMPARE(fileBytes(legacy), legacyBytes);
    struct stat after {};
    QCOMPARE(::stat(QFile::encodeName(legacy).constData(), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(after.st_mtim.tv_sec, before.st_mtim.tv_sec);
    QCOMPARE(after.st_mtim.tv_nsec, before.st_mtim.tv_nsec);
    QCOMPARE(QDir(QFileInfo(legacy).absolutePath()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList {QStringLiteral("PulseScope.conf")});

    // The new file: every key, value and type; private mode; no temporaries.
    QSettings n(fresh, QSettings::IniFormat);
    QCOMPARE(n.status(), QSettings::NoError);
    QSettings o(legacy, QSettings::IniFormat);
    QStringList keys = n.allKeys();
    QStringList want = o.allKeys();
    keys.sort();
    want.sort();
    QCOMPARE(keys, want);
    for (const QString& k : want) {
        QVERIFY2(n.value(k) == o.value(k), qPrintable(k));
        QCOMPARE(n.value(k).typeId(), o.value(k).typeId());
    }
    QCOMPARE(n.value(QStringLiteral("geometry")).typeId(), int(QMetaType::QByteArray));
    QCOMPARE(n.value(QStringLiteral("geometry")).toByteArray(), kGeometry);
    QCOMPARE(n.value(QStringLiteral("Custom/blob")).toByteArray(), kBlob);
    QCOMPARE(n.value(QStringLiteral("Custom/rect")).typeId(), int(QMetaType::QRect));
    QCOMPARE(n.value(QStringLiteral("Custom/rect")).toRect(), kRect);
    QCOMPARE(n.value(QStringLiteral("Custom/list")).toStringList(), kList);
    QCOMPARE(n.value(QStringLiteral("futureKey")).toString(), kFuture);
    QVERIFY(n.contains(QStringLiteral("Custom/Deep/empty")));
    struct stat st {};
    QCOMPARE(::stat(QFile::encodeName(fresh).constData(), &st), 0);
    QVERIFY2((st.st_mode & 0077) == 0, qPrintable(QString::number(st.st_mode & 0777, 8)));
    QCOMPARE(QDir(QFileInfo(fresh).absolutePath()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList {QStringLiteral("bex.conf")});
}

void TestRenameAdversarial::importNeverTouchesExistingNew_data()
{
    QTest::addColumn<QString>("kind");
    for (const char* k : {"file", "empty file", "corrupt file", "directory", "symlink to file", "broken symlink",
                          "symlink loop"})
        QTest::newRow(k) << QString::fromLatin1(k);
}

void TestRenameAdversarial::importNeverTouchesExistingNew()
{
    QFETCH(QString, kind);
    QTemporaryDir d;
    const QString legacy = d.filePath(QStringLiteral("old/PulseScope.conf"));
    const QString fresh = d.filePath(QStringLiteral("new/bex.conf"));
    QVERIFY(writeLegacy(legacy));
    QDir().mkpath(d.filePath(QStringLiteral("new")));
    const QString elsewhere = d.filePath(QStringLiteral("elsewhere.conf"));
    if (kind == QStringLiteral("file"))
        QVERIFY(writeBytes(fresh, "[General]\nintervalMs=2000\nbexOnly=mine\n"));
    else if (kind == QStringLiteral("empty file"))
        QVERIFY(writeBytes(fresh, QByteArray()));
    else if (kind == QStringLiteral("corrupt file"))
        QVERIFY(writeBytes(fresh, QByteArray("\x00\xff[unterminated\n\x01", 17)));
    else if (kind == QStringLiteral("directory"))
        QVERIFY(QDir().mkpath(fresh));
    else if (kind == QStringLiteral("symlink to file")) {
        QVERIFY(writeBytes(elsewhere, "[General]\nx=1\n"));
        QVERIFY(QFile::link(elsewhere, fresh));
    } else if (kind == QStringLiteral("broken symlink"))
        QVERIFY(QFile::link(d.filePath(QStringLiteral("missing.conf")), fresh));
    else if (kind == QStringLiteral("symlink loop"))
        QVERIFY(QFile::link(fresh, fresh));
    const auto before = tree(d.path());
    const settingsimport::Result r = settingsimport::importLegacy(legacy, fresh);
    QVERIFY2(r.outcome == Outcome::NewSettingsExist, qPrintable(describe(r)));
    QCOMPARE(r.keys, 0);
    const auto after = tree(d.path());
    QVERIFY2(after == before, qPrintable(describeTree(before) + QStringLiteral(" -> ") + describeTree(after)));
}

void TestRenameAdversarial::importWithoutLegacyWritesNothing_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("absent") << QStringLiteral("absent") << QByteArray();
    QTest::newRow("absent directory") << QStringLiteral("absent directory") << QByteArray();
    QTest::newRow("0 bytes") << QStringLiteral("file") << QByteArray();
    QTest::newRow("[General] only") << QStringLiteral("file") << QByteArray("[General]\n");
    // Qt INI comments start with ";" only; a "#" line is a FormatError.
    QTest::newRow("comments only") << QStringLiteral("file") << QByteArray("; one\n\n; two\n");
    QTest::newRow("empty key only") << QStringLiteral("file") << QByteArray("[General]\n=value\n");
}

void TestRenameAdversarial::importWithoutLegacyWritesNothing()
{
    QFETCH(QString, kind);
    QFETCH(QByteArray, bytes);
    QTemporaryDir d;
    QString legacy = d.filePath(QStringLiteral("old/PulseScope.conf"));
    if (kind == QStringLiteral("absent"))
        QDir().mkpath(d.filePath(QStringLiteral("old")));
    else if (kind == QStringLiteral("file"))
        QVERIFY(writeBytes(legacy, bytes));
    const QString fresh = d.filePath(QStringLiteral("new/bex.conf"));
    const auto before = tree(d.path());
    const settingsimport::Result r = settingsimport::importLegacy(legacy, fresh);
    QVERIFY2(r.outcome == Outcome::NoLegacySettings, qPrintable(describe(r)));
    QCOMPARE(r.keys, 0);
    // Not even the new directory is created.
    const auto after = tree(d.path());
    QVERIFY2(after == before, qPrintable(describeTree(before) + QStringLiteral(" -> ") + describeTree(after)));
}

void TestRenameAdversarial::importFailuresLeaveNothing_data()
{
    QTest::addColumn<QString>("kind");
    for (const char* k : {"unreadable legacy", "legacy is a directory", "dangling legacy symlink", "binary legacy",
                          "legacy format error", "words without equals", "read-only new directory",
                          "new parent is a file"})
        QTest::newRow(k) << QString::fromLatin1(k);
}

void TestRenameAdversarial::importFailuresLeaveNothing()
{
    // Each case exists but cannot be imported faithfully: Failed with a
    // message, no new file and no temporary left anywhere, the legacy file
    // unchanged. Never a silent partial copy, never "no legacy settings".
    QFETCH(QString, kind);
    const bool root = ::geteuid() == 0;
    if (root && (kind == QStringLiteral("unreadable legacy") || kind == QStringLiteral("read-only new directory")))
        QSKIP("root ignores permissions");
    QTemporaryDir d;
    const QString legacy = d.filePath(QStringLiteral("old/PulseScope.conf"));
    QString fresh = d.filePath(QStringLiteral("new/bex.conf"));
    QString lockDir;
    if (kind == QStringLiteral("legacy is a directory"))
        QVERIFY(QDir().mkpath(legacy + QStringLiteral("/inside")));
    else if (kind == QStringLiteral("dangling legacy symlink")) {
        QVERIFY(QDir().mkpath(d.filePath(QStringLiteral("old"))));
        QVERIFY(QFile::link(d.filePath(QStringLiteral("old/gone.conf")), legacy));
    } else if (kind == QStringLiteral("words without equals"))
        QVERIFY(writeBytes(legacy, "[General]\nintervalMs=5000\nwords without equals\n"));
    else if (kind == QStringLiteral("binary legacy"))
        QVERIFY(writeBytes(legacy, QByteArray("\x7f" "ELF\x02\x01\x01\x00\x00\x00\xff\xfe\x00\x00", 14)));
    else if (kind == QStringLiteral("legacy format error"))
        // Two good keys around a broken section header: importing only the
        // parsable part would silently lose settings.
        QVERIFY(writeBytes(legacy, "[General]\nintervalMs=5000\n[broken\ndetails=true\n"));
    else
        QVERIFY(writeLegacy(legacy));
    if (kind == QStringLiteral("unreadable legacy"))
        QCOMPARE(::chmod(QFile::encodeName(legacy).constData(), 0), 0);
    if (kind == QStringLiteral("read-only new directory")) {
        lockDir = d.filePath(QStringLiteral("new"));
        QVERIFY(QDir().mkpath(lockDir));
        QCOMPARE(::chmod(QFile::encodeName(lockDir).constData(), 0500), 0);
    }
    if (kind == QStringLiteral("new parent is a file")) {
        QVERIFY(writeBytes(d.filePath(QStringLiteral("new")), "a file\n"));
        fresh = d.filePath(QStringLiteral("new/bex.conf"));
    }
    if (kind == QStringLiteral("legacy format error") || kind == QStringLiteral("words without equals")) {
        QTemporaryDir scratch;
        const QString copy = scratch.filePath(QStringLiteral("probe.conf"));
        QVERIFY(writeBytes(copy, fileBytes(legacy)));
        QSettings probe(copy, QSettings::IniFormat);
        (void)probe.allKeys();
        if (probe.status() != QSettings::FormatError)
            QSKIP(qPrintable(QStringLiteral("this Qt parses the broken header without FormatError (status %1, keys %2)")
                                 .arg(int(probe.status()))
                                 .arg(probe.allKeys().join(QLatin1Char(',')))));
    }
    const auto before = tree(d.path());
    const settingsimport::Result r = settingsimport::importLegacy(legacy, fresh);
    const auto after = tree(d.path());
    if (!lockDir.isEmpty())
        ::chmod(QFile::encodeName(lockDir).constData(), 0700);
    if (kind == QStringLiteral("unreadable legacy"))
        ::chmod(QFile::encodeName(legacy).constData(), 0600);
    QVERIFY2(r.outcome == Outcome::Failed, qPrintable(describe(r)));
    QVERIFY2(!r.message.trimmed().isEmpty(), "a failure without a message");
    QVERIFY2(r.message.contains(legacy) || r.message.contains(fresh), qPrintable(r.message));
    QVERIFY2(after == before, qPrintable(describeTree(before) + QStringLiteral(" -> ") + describeTree(after)));
}

void TestRenameAdversarial::importLegacyParentPermissions_data()
{
    // Which directory is restricted, to which mode, whether the legacy file
    // is really there, and the only acceptable outcome.
    QTest::addColumn<QString>("which");
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("present");
    QTest::addColumn<int>("expected");
    const int failed = int(Outcome::Failed);
    // Without search (x) permission on a folder on the path, whether the
    // file exists cannot be known: that is a failure to report, never "no
    // legacy settings", which would let bex save defaults over the chance.
    QTest::newRow("parent 0000, present") << QStringLiteral("parent") << 0000 << true << failed;
    QTest::newRow("parent 0600 (read, no search), present") << QStringLiteral("parent") << 0600 << true << failed;
    QTest::newRow("parent 0200 (write only), present") << QStringLiteral("parent") << 0200 << true << failed;
    QTest::newRow("config dir 0600, present") << QStringLiteral("grandparent") << 0600 << true << failed;
    // Indistinguishable from the case above for this process.
    QTest::newRow("parent 0000, absent") << QStringLiteral("parent") << 0000 << false << failed;
    // Controls: search permission is all a lookup needs.
    QTest::newRow("parent 0100 (search only), present")
        << QStringLiteral("parent") << 0100 << true << int(Outcome::Imported);
    QTest::newRow("parent 0500 (read-only), present")
        << QStringLiteral("parent") << 0500 << true << int(Outcome::Imported);
    QTest::newRow("parent 0100, absent") << QStringLiteral("parent") << 0100 << false << int(Outcome::NoLegacySettings);
    // bex/ cannot be searched: whether bex.conf exists is unknown, so a real
    // legacy file is not imported, and the failure is said; without one,
    // nothing is said.
    QTest::newRow("bex dir 0000, legacy present") << QStringLiteral("new") << 0000 << true << failed;
    QTest::newRow("bex dir 0000, legacy absent")
        << QStringLiteral("new") << 0000 << false << int(Outcome::NoLegacySettings);
}

void TestRenameAdversarial::importLegacyParentPermissions()
{
    QFETCH(QString, which);
    QFETCH(int, mode);
    QFETCH(bool, present);
    QFETCH(int, expected);
    if (::geteuid() == 0 && (expected == int(Outcome::Failed) || mode == 0000))
        QSKIP("root ignores permissions");
    QTemporaryDir d;
    const QString config = d.filePath(QStringLiteral("cfg"));
    const QString parent = config + QStringLiteral("/PulseScope");
    const QString legacy = parent + QStringLiteral("/PulseScope.conf");
    const QString fresh = config + QStringLiteral("/bex/bex.conf");
    QVERIFY(QDir().mkpath(parent));
    if (which == QStringLiteral("new"))
        QVERIFY(QDir().mkpath(QFileInfo(fresh).absolutePath()));
    if (present)
        QVERIFY(writeLegacy(legacy));
    const QByteArray legacyBytes = present ? fileBytes(legacy) : QByteArray();
    const auto before = tree(d.path());
    settingsimport::Result r;
    {
        const QString restricted = which == QStringLiteral("parent")        ? parent
                                   : which == QStringLiteral("grandparent") ? config
                                                                            : QFileInfo(fresh).absolutePath();
        const ModeGuard guard(restricted, mode_t(mode));
        QVERIFY(guard.ok);
        r = settingsimport::importLegacy(legacy, fresh);
    }
    const auto after = tree(d.path());
    QVERIFY2(int(r.outcome) == expected,
             qPrintable(QStringLiteral("expected %1, got %2")
                            .arg(QString::fromLatin1(outcomeName(Outcome(expected))), describe(r))));
    if (present)
        QCOMPARE(fileBytes(legacy), legacyBytes);
    switch (Outcome(expected)) {
    case Outcome::Failed:
        QCOMPARE(r.keys, 0);
        QVERIFY2(r.message.contains(legacy), qPrintable(r.message));
        for (const char* claim : {"no such file", "does not exist", "missing", "no settings", "not a file"})
            QVERIFY2(!r.message.contains(QLatin1String(claim), Qt::CaseInsensitive), qPrintable(r.message));
        QVERIFY2(after == before, qPrintable(describeTree(before) + QStringLiteral(" -> ") + describeTree(after)));
        break;
    case Outcome::NoLegacySettings:
        QCOMPARE(r.keys, 0);
        QVERIFY2(after == before, qPrintable(describeTree(before) + QStringLiteral(" -> ") + describeTree(after)));
        break;
    default: {
        QCOMPARE(r.keys, kLegacyKeys);
        const QSettings s(fresh, QSettings::IniFormat);
        QCOMPARE(s.allKeys().size(), kLegacyKeys);
        QCOMPARE(s.value(QStringLiteral("futureKey")).toString(), kFuture);
        QCOMPARE(s.value(QStringLiteral("geometry")).toByteArray(), kGeometry);
        break;
    }
    }
}

void TestRenameAdversarial::importReadCap_data()
{
    // Total legacy size relative to kMaxLegacyBytes, and the outcome. Every
    // file is a valid settings file whose first kMaxLegacyBytes bytes are a
    // valid settings file too: importing a truncated prefix would pass as
    // Imported, so only Failed shows the cap is enforced.
    QTest::addColumn<qint64>("extra");
    QTest::addColumn<bool>("imported");
    QTest::newRow("exactly the cap") << qint64(0) << true;
    QTest::newRow("cap - 1") << qint64(-1) << true;
    QTest::newRow("cap + 1") << qint64(1) << false;
    // The first kMaxLegacyBytes bytes are the whole valid file without its
    // last group, which starts exactly at the cap.
    QTest::newRow("cap + 18, a group after the cap") << qint64(18) << false;
    QTest::newRow("cap + 4096") << qint64(4096) << false;
}

void TestRenameAdversarial::importReadCap()
{
    QFETCH(qint64, extra);
    QFETCH(bool, imported);
    const bool keyAfterCap = QByteArray(QTest::currentDataTag()).contains("after the cap");
    const qint64 cap = settingsimport::kMaxLegacyBytes;
    QCOMPARE(cap, qint64(8) << 20);
    QTemporaryDir d;
    const QString legacy = d.filePath(QStringLiteral("old/PulseScope.conf"));
    const QString fresh = d.filePath(QStringLiteral("new/bex.conf"));
    QVERIFY(writeLegacy(legacy));
    QByteArray bytes = fileBytes(legacy);
    if (!bytes.endsWith('\n'))
        bytes += '\n';
    // ";" comment lines of 1024 bytes up to the wanted size; the last line
    // is shortened, so the file ends exactly on a newline.
    const qint64 total = cap + extra;
    const QByteArray tailKey = keyAfterCap ? QByteArray("[Tail]\nafterCap=1\n") : QByteArray();
    QVERIFY(bytes.size() + tailKey.size() + 2 <= total);
    bytes.reserve(total);
    while (bytes.size() + tailKey.size() < total) {
        const qint64 room = total - tailKey.size() - bytes.size();
        const qint64 line = std::min<qint64>(1024, room);
        if (line >= 2)
            bytes += ';' + QByteArray(line - 2, 'x') + '\n';
        else
            bytes.insert(bytes.size() - 1, 'x'); // lengthen the last comment by one byte
    }
    bytes += tailKey;
    QCOMPARE(qint64(bytes.size()), total);
    if (keyAfterCap)
        QCOMPARE(qint64(bytes.indexOf("[Tail]")), cap);
    QVERIFY(writeBytes(legacy, bytes));
    QCOMPARE(QFileInfo(legacy).size(), total);
    const QByteArray legacyHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    bytes.clear();
    bytes.squeeze();
    const auto before = tree(d.path());
    const settingsimport::Result r = settingsimport::importLegacy(legacy, fresh);
    const auto after = tree(d.path());
    QCOMPARE(QCryptographicHash::hash(fileBytes(legacy), QCryptographicHash::Sha256), legacyHash);
    if (imported) {
        QVERIFY2(r.outcome == Outcome::Imported, qPrintable(describe(r)));
        QCOMPARE(r.keys, kLegacyKeys);
        const QSettings s(fresh, QSettings::IniFormat);
        QCOMPARE(s.allKeys().size(), kLegacyKeys);
        QCOMPARE(s.value(QStringLiteral("Custom/blob")).toByteArray(), kBlob);
        return;
    }
    QVERIFY2(r.outcome == Outcome::Failed, qPrintable(describe(r)));
    QCOMPARE(r.keys, 0);
    QVERIFY2(r.message.contains(legacy), qPrintable(r.message));
    QVERIFY2(r.message.contains(QStringLiteral("8 MiB")), qPrintable(r.message));
    for (const char* claim : {"no such file", "does not exist", "missing", "no settings", "damaged"})
        QVERIFY2(!r.message.contains(QLatin1String(claim), Qt::CaseInsensitive), qPrintable(r.message));
    QVERIFY2(after == before, qPrintable(describeTree(before) + QStringLiteral(" -> ") + describeTree(after)));
}

void TestRenameAdversarial::importIsNotRepeated()
{
    QTemporaryDir d;
    const QString legacy = d.filePath(QStringLiteral("PulseScope.conf"));
    const QString fresh = d.filePath(QStringLiteral("bex/bex.conf"));
    QVERIFY(writeLegacy(legacy));
    QVERIFY2(settingsimport::importLegacy(legacy, fresh).outcome == Outcome::Imported, "first import");
    {
        QSettings n(fresh, QSettings::IniFormat);
        n.remove(QStringLiteral("Custom"));
        n.setValue(QStringLiteral("intervalMs"), 500);
        n.sync();
    }
    const QByteArray changed = fileBytes(fresh);
    const settingsimport::Result again = settingsimport::importLegacy(legacy, fresh);
    QVERIFY2(again.outcome == Outcome::NewSettingsExist, qPrintable(describe(again)));
    QCOMPARE(fileBytes(fresh), changed);
}

void TestRenameAdversarial::defaultPathsFollowXdg()
{
    // This process runs with XDG_CONFIG_HOME = <private>/cfg (see main).
    const QString cfg = g_privateRoot + QStringLiteral("/cfg");
    QCOMPARE(settingsimport::legacyFile(), cfg + QStringLiteral("/PulseScope/PulseScope.conf"));
    const QString org = QCoreApplication::organizationName(), app = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("bex"));
    QCoreApplication::setApplicationName(QStringLiteral("bex"));
    const QString def = settingsimport::defaultFile();
    QCoreApplication::setOrganizationName(org);
    QCoreApplication::setApplicationName(app);
    QCOMPARE(def, cfg + QStringLiteral("/bex/bex.conf"));
    // Asking for the paths creates nothing.
    QVERIFY2(!QFileInfo::exists(cfg + QStringLiteral("/bex")), "defaultFile() created a directory");
    QVERIFY2(!QFileInfo::exists(cfg + QStringLiteral("/PulseScope")), "legacyFile() created a directory");
}

void TestRenameAdversarial::cliExitsWriteNothing_data()
{
    QTest::addColumn<QStringList>("args");
    QTest::addColumn<int>("exitCode");
    QTest::newRow("--help") << QStringList {QStringLiteral("--help")} << 0;
    QTest::newRow("--version") << QStringList {QStringLiteral("--version")} << 0;
    QTest::newRow("--theme-info") << QStringList {QStringLiteral("--theme-info")} << 0;
    QTest::newRow("bad --interval") << QStringList {QStringLiteral("--interval"), QStringLiteral("3")} << 2;
    QTest::newRow("bad --page") << QStringList {QStringLiteral("--page"), QStringLiteral("nope")} << 2;
    QTest::newRow("bad --size") << QStringList {QStringLiteral("--size"), QStringLiteral("1x1")} << 2;
    QTest::newRow("bad --quit-after") << QStringList {QStringLiteral("--quit-after"), QStringLiteral("-1")} << 2;
}

void TestRenameAdversarial::cliExitsWriteNothing()
{
    if (appPath().isEmpty())
        QSKIP("bex binary not found (set RN_APP)");
    QFETCH(QStringList, args);
    QFETCH(int, exitCode);
    for (const bool withLegacy : {true, false}) {
        PrivateHome h;
        if (withLegacy)
            QVERIFY(writeLegacy(h.legacy()));
        const auto before = tree(h.config());
        const Run r = runApp(h, args);
        QVERIFY2(r.finished, qPrintable(r.err));
        QVERIFY2(r.exitCode == exitCode, qPrintable(QStringLiteral("exit %1: %2").arg(r.exitCode).arg(r.err)));
        QVERIFY2(sanitizerSilent(r.err), qPrintable(r.err));
        const auto after = tree(h.config());
        QVERIFY2(after == before, qPrintable(QStringLiteral("%1 (legacy %2) changed the configuration: %3 -> %4")
                                                 .arg(args.join(QLatin1Char(' ')))
                                                 .arg(withLegacy)
                                                 .arg(describeTree(before), describeTree(after))));
        QVERIFY2(!r.err.contains(QStringLiteral("import"), Qt::CaseInsensitive), qPrintable(r.err));
    }
}

void TestRenameAdversarial::versionHelpAndErrorsSayBex()
{
    if (appPath().isEmpty())
        QSKIP("bex binary not found (set RN_APP)");
    QCOMPARE(QFileInfo(appPath()).fileName(), QStringLiteral("bex"));
    PrivateHome h;
    const Run v = runApp(h, {QStringLiteral("--version")});
    QVERIFY2(v.finished && v.exitCode == 0, qPrintable(v.err));
    QVERIFY2(QRegularExpression(QStringLiteral("^bex \\d+\\.\\d+\\.\\d+\\n?$")).match(v.out).hasMatch(),
             qPrintable(QStringLiteral("--version printed \"%1\"").arg(v.out)));
    const Run help = runApp(h, {QStringLiteral("--help")});
    QVERIFY2(help.finished && help.exitCode == 0, qPrintable(help.err));
    // "Usage: <argv0>" names wherever the binary was built, which may be a
    // checkout folder still called pulsescope: only the app's own text counts.
    QString helpText = help.out;
    helpText.replace(appPath(), QStringLiteral("<app>"));
    QVERIFY2(helpText.contains(QStringLiteral("Usage: <app>")), qPrintable(help.out));
    QVERIFY2(!helpText.contains(QStringLiteral("pulsescope"), Qt::CaseInsensitive),
             qPrintable(QStringLiteral("--help still names PulseScope:\n%1").arg(help.out)));
    const Run bad = runApp(h, {QStringLiteral("--page"), QStringLiteral("nope")});
    QCOMPARE(bad.exitCode, 2);
    QVERIFY2(bad.err.startsWith(QStringLiteral("bex: ")), qPrintable(QStringLiteral("stderr \"%1\"").arg(bad.err.trimmed())));
    QVERIFY2(!QString(bad.err).replace(appPath(), QStringLiteral("<app>")).contains(QStringLiteral("pulsescope"), Qt::CaseInsensitive),
             qPrintable(bad.err));
}

void TestRenameAdversarial::startupImportsOnceAndReports()
{
    if (appPath().isEmpty())
        QSKIP("bex binary not found (set RN_APP)");
    PrivateHome h;
    QVERIFY(writeLegacy(h.legacy()));
    const QByteArray legacyBytes = fileBytes(h.legacy());
    const Run r = startAndClose(h);
    QVERIFY2(r.finished && r.exitCode == 0, qPrintable(QStringLiteral("exit %1: %2").arg(r.exitCode).arg(r.err)));
    QVERIFY2(sanitizerSilent(r.err), qPrintable(r.err));
    QCOMPARE(fileBytes(h.legacy()), legacyBytes);
    QVERIFY2(QFileInfo(h.fresh()).isFile(), qPrintable(describeTree(tree(h.config()))));
    const QStringList lines = bexLines(r.err);
    QVERIFY2(lines.size() == 1 && lines.front().contains(h.legacy()) && lines.front().contains(h.fresh()),
             qPrintable(QStringLiteral("stderr: \"%1\"").arg(r.err.trimmed())));

    QSettings s(h.fresh(), QSettings::IniFormat);
    QCOMPARE(s.status(), QSettings::NoError);
    // The app read the imported values at start-up and saved them again at
    // close: not its defaults (1000 ms, 300 s, simple view, smooth graphs).
    QCOMPARE(s.value(QStringLiteral("intervalMs")).toInt(), 5000);
    QCOMPARE(s.value(QStringLiteral("windowSeconds")).toInt(), 900);
    QCOMPARE(s.value(QStringLiteral("details")).toBool(), true);
    QCOMPARE(s.value(QStringLiteral("smoothGraphs")).toBool(), false);
    // Values the app never writes keep their text and their types.
    QCOMPARE(s.value(QStringLiteral("futureKey")).toString(), kFuture);
    QCOMPARE(s.value(QStringLiteral("Custom/blob")).typeId(), int(QMetaType::QByteArray));
    QCOMPARE(s.value(QStringLiteral("Custom/blob")).toByteArray(), kBlob);
    QCOMPARE(s.value(QStringLiteral("Custom/rect")).typeId(), int(QMetaType::QRect));
    QCOMPARE(s.value(QStringLiteral("Custom/rect")).toRect(), kRect);
    QCOMPARE(s.value(QStringLiteral("Custom/list")).toStringList(), kList);
    QCOMPARE(s.value(QStringLiteral("geometry")).typeId(), int(QMetaType::QByteArray));
    // Only bex/ appeared next to PulseScope/, and PulseScope/ holds only
    // its file.
    QStringList top = QDir(h.config()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    top.sort();
    QCOMPARE(top, (QStringList {QStringLiteral("PulseScope"), QStringLiteral("bex")}));
    QCOMPARE(QDir(QFileInfo(h.legacy()).absolutePath()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
             QStringList {QStringLiteral("PulseScope.conf")});
}

void TestRenameAdversarial::existingBexSettingsWin()
{
    if (appPath().isEmpty())
        QSKIP("bex binary not found (set RN_APP)");
    PrivateHome h;
    QVERIFY(writeLegacy(h.legacy()));
    QVERIFY(writeBytes(h.fresh(), "[General]\nintervalMs=2000\nbexOnly=mine\n"));
    const QByteArray legacyBytes = fileBytes(h.legacy());
    const Run r = startAndClose(h);
    QVERIFY2(r.finished && r.exitCode == 0, qPrintable(r.err));
    QVERIFY2(bexLines(r.err).isEmpty(), qPrintable(r.err));
    QCOMPARE(fileBytes(h.legacy()), legacyBytes);
    QSettings s(h.fresh(), QSettings::IniFormat);
    QCOMPARE(s.value(QStringLiteral("intervalMs")).toInt(), 2000);
    QCOMPARE(s.value(QStringLiteral("bexOnly")).toString(), QStringLiteral("mine"));
    QVERIFY(!s.contains(QStringLiteral("futureKey")));
    QVERIFY(!s.contains(QStringLiteral("Custom/blob")));
    QCOMPARE(s.value(QStringLiteral("windowSeconds"), -1).toInt(), 300); // the default, saved at close
}

void TestRenameAdversarial::laterStartDoesNotReimport()
{
    if (appPath().isEmpty())
        QSKIP("bex binary not found (set RN_APP)");
    PrivateHome h;
    QVERIFY(writeLegacy(h.legacy()));
    const QByteArray legacyBytes = fileBytes(h.legacy());
    Run r = startAndClose(h);
    QVERIFY2(r.finished && r.exitCode == 0, qPrintable(r.err));
    {
        QSettings s(h.fresh(), QSettings::IniFormat);
        QVERIFY(s.contains(QStringLiteral("futureKey")));
        s.remove(QStringLiteral("futureKey"));
        s.remove(QStringLiteral("Custom"));
        s.setValue(QStringLiteral("intervalMs"), 500);
        s.sync();
    }
    r = startAndClose(h);
    QVERIFY2(r.finished && r.exitCode == 0, qPrintable(r.err));
    QVERIFY2(bexLines(r.err).isEmpty(), qPrintable(r.err));
    QSettings s(h.fresh(), QSettings::IniFormat);
    QCOMPARE(s.value(QStringLiteral("intervalMs")).toInt(), 500);
    QVERIFY(!s.contains(QStringLiteral("futureKey")));
    QVERIFY(!s.contains(QStringLiteral("Custom/blob")));
    QCOMPARE(fileBytes(h.legacy()), legacyBytes);
}

void TestRenameAdversarial::noLegacyStartsFresh()
{
    if (appPath().isEmpty())
        QSKIP("bex binary not found (set RN_APP)");
    PrivateHome h;
    const Run r = startAndClose(h);
    QVERIFY2(r.finished && r.exitCode == 0, qPrintable(r.err));
    QVERIFY2(bexLines(r.err).isEmpty(), qPrintable(r.err));
    QVERIFY(!QFileInfo::exists(QFileInfo(h.legacy()).absolutePath()));
    QSettings s(h.fresh(), QSettings::IniFormat);
    QCOMPARE(s.value(QStringLiteral("intervalMs")).toInt(), 1000);
    QVERIFY(!s.contains(QStringLiteral("futureKey")));
}

void TestRenameAdversarial::blockedTargetIsReportedAndLegacyKept()
{
    // A plain file named "bex" where the settings directory belongs: the
    // import cannot happen. That is said on stderr, nothing is lost, and
    // the blocking file is not replaced.
    if (appPath().isEmpty())
        QSKIP("bex binary not found (set RN_APP)");
    PrivateHome h;
    QVERIFY(writeLegacy(h.legacy()));
    QVERIFY(writeBytes(h.config() + QStringLiteral("/bex"), "not a directory\n"));
    const QByteArray legacyBytes = fileBytes(h.legacy());
    const Run r = startAndClose(h);
    QVERIFY2(r.finished, qPrintable(r.err));
    QVERIFY2(sanitizerSilent(r.err), qPrintable(r.err));
    QCOMPARE(fileBytes(h.legacy()), legacyBytes);
    QCOMPARE(fileBytes(h.config() + QStringLiteral("/bex")), QByteArray("not a directory\n"));
    const QStringList lines = bexLines(r.err);
    QVERIFY2(!lines.isEmpty() && lines.front().contains(h.legacy()),
             qPrintable(QStringLiteral("no report of the failed import: \"%1\"").arg(r.err.trimmed())));
    QVERIFY2(!r.err.contains(QStringLiteral("imported"), Qt::CaseInsensitive) ||
                 r.err.contains(QStringLiteral("not imported"), Qt::CaseInsensitive) ||
                 r.err.contains(QStringLiteral("could not"), Qt::CaseInsensitive),
             qPrintable(r.err));
}

void TestRenameAdversarial::unreadableLegacyIsReportedNotImported()
{
    // A legacy file that exists but cannot be read is a failure to report,
    // not "no legacy settings".
    if (appPath().isEmpty())
        QSKIP("bex binary not found (set RN_APP)");
    if (::geteuid() == 0)
        QSKIP("root reads any file");
    PrivateHome h;
    QVERIFY(writeLegacy(h.legacy()));
    const QByteArray legacyBytes = fileBytes(h.legacy());
    QCOMPARE(::chmod(QFile::encodeName(h.legacy()).constData(), 0), 0);
    const Run r = startAndClose(h);
    ::chmod(QFile::encodeName(h.legacy()).constData(), 0600);
    QVERIFY2(r.finished && r.exitCode == 0, qPrintable(r.err));
    QVERIFY2(sanitizerSilent(r.err), qPrintable(r.err));
    QCOMPARE(fileBytes(h.legacy()), legacyBytes);
    const QStringList lines = bexLines(r.err);
    QVERIFY2(lines.size() == 1 && lines.front().contains(h.legacy()),
             qPrintable(QStringLiteral("no report of the unreadable legacy settings: \"%1\"").arg(r.err.trimmed())));
    // bex ran on its defaults (and saved them), never on invented values.
    QSettings s(h.fresh(), QSettings::IniFormat);
    QCOMPARE(s.value(QStringLiteral("intervalMs"), 1000).toInt(), 1000);
    QVERIFY(!s.contains(QStringLiteral("futureKey")));
}

void TestRenameAdversarial::unsearchableLegacyFolderIsReported()
{
    // The PulseScope folder exists but cannot be searched: bex cannot know
    // whether settings are there, says so once, and never reports "none".
    if (appPath().isEmpty())
        QSKIP("bex binary not found (set RN_APP)");
    if (::geteuid() == 0)
        QSKIP("root ignores permissions");
    PrivateHome h;
    QVERIFY(writeLegacy(h.legacy()));
    const QByteArray legacyBytes = fileBytes(h.legacy());
    Run r;
    {
        const ModeGuard guard(QFileInfo(h.legacy()).absolutePath(), 0000);
        QVERIFY(guard.ok);
        r = startAndClose(h);
    }
    QVERIFY2(r.finished && r.exitCode == 0, qPrintable(r.err));
    QVERIFY2(sanitizerSilent(r.err), qPrintable(r.err));
    QCOMPARE(fileBytes(h.legacy()), legacyBytes);
    const QStringList lines = bexLines(r.err);
    QVERIFY2(lines.size() == 1 && lines.front().contains(h.legacy()),
             qPrintable(QStringLiteral("no single report of the unsearchable legacy folder: \"%1\"").arg(r.err.trimmed())));
    QVERIFY2(!lines.front().contains(QStringLiteral("Imported"), Qt::CaseInsensitive), qPrintable(lines.front()));
    QSettings s(h.fresh(), QSettings::IniFormat);
    QVERIFY(!s.contains(QStringLiteral("futureKey")));
}

void TestRenameAdversarial::stagedInstallIdentity()
{
    const QString stage = qEnvironmentVariable("RN_STAGE");
    if (stage.isEmpty())
        QSKIP("no staged install tree (set RN_STAGE to DESTDIR+prefix)");
    const QString id = QStringLiteral("io.github.kidusm001.bex");
    const QString bin = stage + QStringLiteral("/bin/bex");
    const QString desktop = stage + QStringLiteral("/share/applications/") + id + QStringLiteral(".desktop");
    const QString meta = stage + QStringLiteral("/share/metainfo/") + id + QStringLiteral(".metainfo.xml");
    const QString icon = stage + QStringLiteral("/share/icons/hicolor/scalable/apps/") + id + QStringLiteral(".svg");
    for (const QString& f : {bin, desktop, meta, icon})
        QVERIFY2(QFileInfo(f).isFile(), qPrintable(f + QStringLiteral(" is missing")));
    QVERIFY(QFileInfo(bin).isExecutable());
    QDirIterator it(stage, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        QVERIFY2(!QFileInfo(p).fileName().contains(QStringLiteral("pulsescope"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("installed under an old name: ") + p));
    }

    QSettings entry(desktop, QSettings::IniFormat);
    entry.beginGroup(QStringLiteral("Desktop Entry"));
    QCOMPARE(entry.value(QStringLiteral("Name")).toString(), QStringLiteral("bex"));
    QCOMPARE(entry.value(QStringLiteral("Icon")).toString(), id);
    QCOMPARE(entry.value(QStringLiteral("Type")).toString(), QStringLiteral("Application"));
    const QString exec = entry.value(QStringLiteral("Exec")).toString();
    QVERIFY2(exec.endsWith(QStringLiteral("/bin/bex")) || exec.endsWith(QStringLiteral("/bin/bex\"")),
             qPrintable(QStringLiteral("Exec=") + exec));
    const QString wmClass = entry.value(QStringLiteral("StartupWMClass")).toString();
    if (!wmClass.isEmpty())
        QCOMPARE(wmClass, id);
    // The Exec line holds the install prefix, which is not the app's text.
    QByteArray desktopText = fileBytes(desktop);
    desktopText.replace(QFile::encodeName(exec), "<exec>");
    QVERIFY2(!desktopText.toLower().contains("pulsescope"), desktopText.constData());

    QFile metaFile(meta);
    QVERIFY(metaFile.open(QIODevice::ReadOnly));
    QXmlStreamReader xml(&metaFile);
    QString metaId, launchable, name;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.name() == QStringLiteral("id") && metaId.isEmpty())
            metaId = xml.readElementText();
        else if (xml.name() == QStringLiteral("launchable"))
            launchable = xml.readElementText();
        else if (xml.name() == QStringLiteral("name") && name.isEmpty() &&
                 xml.attributes().value(QStringLiteral("xml:lang")).isEmpty())
            name = xml.readElementText();
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
    QCOMPARE(metaId, id);
    QCOMPARE(launchable, id + QStringLiteral(".desktop"));
    QCOMPARE(name, QStringLiteral("bex"));
    QVERIFY2(!fileBytes(meta).toLower().contains("pulsescope"), "the AppStream metadata still names PulseScope");
    QVERIFY(fileBytes(icon).contains("<svg"));
}

int main(int argc, char** argv)
{
    // Before Qt reads any path: this process's HOME and XDG_* are private.
    char tmpl[] = "/tmp/bex-rename-qa-XXXXXX";
    const char* made = ::mkdtemp(tmpl);
    if (!made)
        return 3;
    g_privateRoot = QString::fromLocal8Bit(made);
    const QByteArray cfg = QByteArray(made) + "/cfg";
    ::mkdir(cfg.constData(), 0700);
    ::setenv("HOME", made, 1);
    ::setenv("XDG_CONFIG_HOME", cfg.constData(), 1);
    ::unsetenv("XDG_CONFIG_DIRS");
    int rc = 0;
    {
        QCoreApplication app(argc, argv);
        TestRenameAdversarial tc;
        QTEST_SET_MAIN_SOURCE_PATH
        rc = QTest::qExec(&tc, argc, argv);
        QDir(g_privateRoot).removeRecursively();
    }
    return rc;
}

#include "tst_rename_adversarial.moc"
