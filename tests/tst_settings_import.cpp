// Settings import from PulseScope, bex's former name, and the renamed app's
// identity. Every file lives in a private HOME and configuration folder
// made before QApplication starts; the user's own settings are never read.
#include "app/mainwindow.h"
#include "app/settingsimport.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstdlib>
#include <memory>
#include <unistd.h>

using settingsimport::Outcome;

namespace {

std::unique_ptr<QTemporaryDir> g_root; // HOME and XDG_CONFIG_HOME live here

QString configDir()
{
    return g_root->filePath(QStringLiteral("config"));
}

// PulseScope's file as it wrote it: typed values, a group and a list.
const char kLegacy[] =
    "[General]\n"
    "details=true\n"
    "disk=legacydisk0\n"
    "geometry=@ByteArray(\\x1\\xd9\\xd0\\xcb\\0\\x3\\0\\0\\0\\0\\x2\\x4\\0\\0\\x6@)\n"
    "interface=legacynet0\n"
    "intervalMs=5000\n"
    "page=5\n"
    "sensors=coretemp/Package id 0, nvme/Composite\n"
    "smoothGraphs=false\n"
    "windowSeconds=900\n"
    "\n"
    "[processView]\n"
    "expandKernel=true\n"
    "sortColumn=2\n"
    "sortOrder=0\n";
constexpr int kLegacyKeys = 12;

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size() && f.flush();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
}

// Every entry under dir (symbolic links not followed) with its content hash.
QStringList tree(const QString& dir)
{
    QStringList out;
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        const QString rel = QDir(dir).relativeFilePath(info.filePath());
        if (info.isSymLink())
            out << rel + QStringLiteral(" -> ") + info.symLinkTarget();
        else if (info.isDir())
            out << rel + QLatin1Char('/');
        else
            out << rel + QLatin1Char(' ') +
                       QString::fromLatin1(QCryptographicHash::hash(readFile(info.filePath()), QCryptographicHash::Sha256).toHex());
    }
    out.sort();
    return out;
}

// Every value of a settings file, parsed from disk under a path this
// process has never read (QSettings caches files by path).
QVariantMap parsed(const QString& path)
{
    static int n = 0;
    const QString copy = g_root->filePath(QStringLiteral("parsed-%1.conf").arg(++n));
    if (!QFile::copy(path, copy))
        return {{QStringLiteral("<error>"), QStringLiteral("cannot copy %1").arg(path)}};
    QVariantMap values;
    {
        const QSettings s(copy, QSettings::NativeFormat);
        for (const QString& key : s.allKeys())
            values.insert(key, s.value(key));
    }
    QFile::remove(copy);
    return values;
}

void clearConfig()
{
    QDir(configDir()).removeRecursively();
    QDir().mkpath(configDir());
}

// Sets a folder's permission bits and restores them when destroyed.
struct Chmod {
    QString path;
    QFileDevice::Permissions saved;
    Chmod(const QString& p, QFileDevice::Permissions mode)
        : path(p)
        , saved(QFile::permissions(p))
    {
        QFile::setPermissions(path, mode);
    }
    ~Chmod() { QFile::setPermissions(path, saved); }
};

struct AppRun {
    int exitCode = -1;
    QString out;
    QString err;
};

AppRun runApp(const QStringList& args)
{
    AppRun r;
    const QString app = qEnvironmentVariable("BEX_APP");
    if (app.isEmpty())
        return r;
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment(); // HOME and XDG are private
    for (const char* var : {"DISPLAY", "WAYLAND_DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "DBUS_SESSION_BUS_ADDRESS",
                            "QT_QPA_PLATFORMTHEME", "QT_STYLE_OVERRIDE"})
        env.remove(QString::fromLatin1(var));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    p.setProcessEnvironment(env);
    p.start(app, args);
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished(5000);
        r.err = QStringLiteral("<timed out>");
        return r;
    }
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = QString::fromLocal8Bit(p.readAllStandardOutput());
    r.err = QString::fromLocal8Bit(p.readAllStandardError());
    return r;
}

} // namespace

class TestSettingsImport : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void init();
    void cleanupTestCase();
    void pathsFollowXdgConfigHome();
    void freshInstallCreatesNothing();
    void importsEveryTypedValueOnce();
    void currentSettingsWin();
    void failuresAreReportedAndChangeNothing_data();
    void failuresAreReportedAndChangeNothing();
    void sizeLimit();
    void foldersThatCannotBeSearched_data();
    void foldersThatCannotBeSearched();
    void legacyLinks();
    void explicitSettingsFileNeverImports();
    void appIdentityAndCliTouchNoSettings();
    void appImportsOnceAtStartup();
    void appReportsFailedImport();
};

void TestSettingsImport::init()
{
    clearConfig();
}

void TestSettingsImport::cleanupTestCase()
{
    // A read-only folder left by a failed case would stop the removal.
    QDirIterator it(g_root->path(), QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        QFile::setPermissions(it.next(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

void TestSettingsImport::pathsFollowXdgConfigHome()
{
    QCOMPARE(settingsimport::defaultFile(), configDir() + QStringLiteral("/bex/bex.conf"));
    QCOMPARE(settingsimport::legacyFile(), configDir() + QStringLiteral("/PulseScope/PulseScope.conf"));
    QCOMPARE(tree(configDir()), QStringList());
}

void TestSettingsImport::freshInstallCreatesNothing()
{
    const auto r = settingsimport::importLegacy(settingsimport::legacyFile(), settingsimport::defaultFile());
    QCOMPARE(r.outcome, Outcome::NoLegacySettings);
    QVERIFY(r.message.isEmpty());
    QCOMPARE(tree(configDir()), QStringList());

    // An empty or key-less legacy file holds nothing to import either.
    for (const QByteArray& content : {QByteArray(), QByteArray("[General]\n"), QByteArray("; comment\n")}) {
        QVERIFY(writeFile(settingsimport::legacyFile(), content));
        const QStringList before = tree(configDir());
        QCOMPARE(settingsimport::importLegacy(settingsimport::legacyFile(), settingsimport::defaultFile()).outcome,
                 Outcome::NoLegacySettings);
        QCOMPARE(tree(configDir()), before);
    }
}

void TestSettingsImport::importsEveryTypedValueOnce()
{
    const QString legacy = settingsimport::legacyFile();
    const QString current = settingsimport::defaultFile();
    QVERIFY(writeFile(legacy, kLegacy));
    const QVariantMap expected = parsed(legacy);
    QCOMPARE(expected.size(), kLegacyKeys);
    QCOMPARE(expected.value(QStringLiteral("geometry")).typeId(), int(QMetaType::QByteArray));
    QCOMPARE(expected.value(QStringLiteral("sensors")).typeId(), int(QMetaType::QStringList));
    const QFileInfo legacyBefore(legacy);

    const auto r = settingsimport::importLegacy(legacy, current);
    QCOMPARE(r.outcome, Outcome::Imported);
    QCOMPARE(r.keys, kLegacyKeys);
    QVERIFY2(r.message.contains(legacy) && r.message.contains(current), qPrintable(r.message));

    const QVariantMap imported = parsed(current);
    QCOMPARE(imported.keys(), expected.keys());
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        QCOMPARE(imported.value(it.key()).typeId(), it.value().typeId());
        QCOMPARE(imported.value(it.key()), it.value());
    }
    QCOMPARE(readFile(legacy), QByteArray(kLegacy));
    QCOMPARE(QFileInfo(legacy).lastModified(), legacyBefore.lastModified());
    // Only the two settings files: no temporary or lock file is left.
    QCOMPARE(QDir(configDir() + QStringLiteral("/bex")).entryList(QDir::Files | QDir::Hidden), QStringList {QStringLiteral("bex.conf")});
    QCOMPARE(QDir(configDir() + QStringLiteral("/PulseScope")).entryList(QDir::Files | QDir::Hidden),
             QStringList {QStringLiteral("PulseScope.conf")});

    // Choices made after the import are never overwritten by another one.
    QVERIFY(writeFile(current, "[General]\nintervalMs=2000\n"));
    const QStringList before = tree(configDir());
    const auto again = settingsimport::importLegacy(legacy, current);
    QCOMPARE(again.outcome, Outcome::NewSettingsExist);
    QVERIFY(again.message.isEmpty());
    QCOMPARE(tree(configDir()), before);
}

void TestSettingsImport::currentSettingsWin()
{
    const QString legacy = settingsimport::legacyFile();
    const QString current = settingsimport::defaultFile();
    QVERIFY(writeFile(legacy, kLegacy));
    // Whatever is there wins, even an empty or damaged file.
    for (const QByteArray& content : {QByteArray("[General]\nintervalMs=2000\n"), QByteArray(), QByteArray("junk\x01")}) {
        QVERIFY(writeFile(current, content));
        const QStringList before = tree(configDir());
        QCOMPARE(settingsimport::importLegacy(legacy, current).outcome, Outcome::NewSettingsExist);
        QCOMPARE(tree(configDir()), before);
    }
}

void TestSettingsImport::failuresAreReportedAndChangeNothing_data()
{
    QTest::addColumn<QByteArray>("legacy"); // empty: a folder instead of a file
    QTest::addColumn<bool>("unreadable");
    QTest::addColumn<bool>("readOnlyTarget");
    QTest::newRow("damaged") << QByteArray("[General]\nintervalMs=5000\nwords without equals\n") << false << false;
    QTest::newRow("binary") << QByteArray("\x7f" "ELF\x02\x01\x01\0\0\0\xff\xfe", 12) << false << false;
    QTest::newRow("folder") << QByteArray() << false << false;
    QTest::newRow("unreadable") << QByteArray(kLegacy) << true << false;
    QTest::newRow("read-only target folder") << QByteArray(kLegacy) << false << true;
}

void TestSettingsImport::failuresAreReportedAndChangeNothing()
{
    QFETCH(QByteArray, legacy);
    QFETCH(bool, unreadable);
    QFETCH(bool, readOnlyTarget);
    if ((unreadable || readOnlyTarget) && ::geteuid() == 0)
        QSKIP("file permissions do not apply to root");
    const QString legacyPath = settingsimport::legacyFile();
    const QString current = settingsimport::defaultFile();
    if (legacy.isEmpty())
        QVERIFY(QDir().mkpath(legacyPath));
    else
        QVERIFY(writeFile(legacyPath, legacy));
    if (unreadable)
        QVERIFY(QFile::setPermissions(legacyPath, QFileDevice::WriteOwner));
    const QString targetDir = QFileInfo(current).absolutePath();
    if (readOnlyTarget) {
        QVERIFY(QDir().mkpath(targetDir));
        QVERIFY(QFile::setPermissions(targetDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    }
    const QStringList before = tree(configDir());

    // The same answer every time: nothing earlier in this process is reused.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto r = settingsimport::importLegacy(legacyPath, current);
        QCOMPARE(r.outcome, Outcome::Failed);
        QCOMPARE(r.keys, 0);
        QVERIFY2(r.message.contains(legacyPath) && r.message.contains(QStringLiteral("unchanged")), qPrintable(r.message));
        QVERIFY(!QFileInfo::exists(current));
        QCOMPARE(tree(configDir()), before);
    }
    if (unreadable)
        QFile::setPermissions(legacyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (readOnlyTarget)
        QFile::setPermissions(targetDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

void TestSettingsImport::sizeLimit()
{
    const QString legacy = settingsimport::legacyFile();
    const QString current = settingsimport::defaultFile();
    const QByteArray head("[General]\nbig=");
    QByteArray bytes = head + QByteArray(settingsimport::kMaxLegacyBytes - head.size() - 1, 'a') + '\n';
    QCOMPARE(bytes.size(), settingsimport::kMaxLegacyBytes);

    // Exactly the limit is imported whole.
    QVERIFY(writeFile(legacy, bytes));
    const auto r = settingsimport::importLegacy(legacy, current);
    QCOMPARE(r.outcome, Outcome::Imported);
    QCOMPARE(r.keys, 1);
    QCOMPARE(parsed(current).value(QStringLiteral("big")).toString().size(), bytes.size() - head.size() - 1);

    // One byte more is refused, and no part of it is imported.
    QVERIFY(QFile::remove(current));
    bytes.insert(head.size(), 'a');
    QVERIFY(writeFile(legacy, bytes));
    const QStringList before = tree(configDir());
    const auto over = settingsimport::importLegacy(legacy, current);
    QCOMPARE(over.outcome, Outcome::Failed);
    QVERIFY2(over.message.contains(legacy) && over.message.contains(QStringLiteral("larger than 8 MiB")),
             qPrintable(over.message));
    QCOMPARE(tree(configDir()), before);
    QCOMPARE(readFile(legacy), bytes);
}

void TestSettingsImport::foldersThatCannotBeSearched_data()
{
    QTest::addColumn<QString>("folder"); // under the configuration folder; empty: that folder itself
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("legacyPresent");
    QTest::addColumn<Outcome>("expected");
    const auto none = int(QFileDevice::Permissions {});
    const auto rw = int(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto x = int(QFileDevice::ExeOwner);
    QTest::newRow("legacy folder closed") << QStringLiteral("PulseScope") << none << true << Outcome::Failed;
    QTest::newRow("legacy folder closed, no file") << QStringLiteral("PulseScope") << none << false << Outcome::Failed;
    QTest::newRow("legacy folder not searchable") << QStringLiteral("PulseScope") << rw << true << Outcome::Failed;
    QTest::newRow("configuration folder not searchable") << QString() << rw << true << Outcome::Failed;
    QTest::newRow("bex folder not searchable") << QStringLiteral("bex") << rw << true << Outcome::Failed;
    // Search permission is enough to find the file, or that it is missing.
    QTest::newRow("legacy folder search only") << QStringLiteral("PulseScope") << x << true << Outcome::Imported;
    QTest::newRow("legacy folder search only, no file")
        << QStringLiteral("PulseScope") << x << false << Outcome::NoLegacySettings;
    // Without a legacy file there is nothing to warn about.
    QTest::newRow("bex folder not searchable, no legacy file")
        << QStringLiteral("bex") << rw << false << Outcome::NoLegacySettings;
}

void TestSettingsImport::foldersThatCannotBeSearched()
{
    QFETCH(QString, folder);
    QFETCH(int, mode);
    QFETCH(bool, legacyPresent);
    QFETCH(Outcome, expected);
    if (::geteuid() == 0)
        QSKIP("file permissions do not apply to root");
    const QString legacy = settingsimport::legacyFile();
    const QString current = settingsimport::defaultFile();
    QVERIFY(QDir().mkpath(QFileInfo(legacy).absolutePath()));
    QVERIFY(QDir().mkpath(QFileInfo(current).absolutePath()));
    if (legacyPresent)
        QVERIFY(writeFile(legacy, kLegacy));
    const QStringList before = tree(configDir());
    settingsimport::Result r;
    {
        const Chmod closed(QDir(configDir()).filePath(folder), QFileDevice::Permissions(mode));
        r = settingsimport::importLegacy(legacy, current);
    }
    QCOMPARE(r.outcome, expected);
    if (expected == Outcome::Imported) {
        QCOMPARE(parsed(current).size(), kLegacyKeys);
    } else {
        QCOMPARE(tree(configDir()), before);
    }
    if (expected == Outcome::Failed)
        QVERIFY2(r.message.contains(legacy) && r.message.contains(QStringLiteral("cannot be checked")),
                 qPrintable(r.message));
    if (legacyPresent)
        QCOMPARE(readFile(legacy), QByteArray(kLegacy));
}

void TestSettingsImport::legacyLinks()
{
    const QString legacy = settingsimport::legacyFile();
    const QString current = settingsimport::defaultFile();
    QVERIFY(QDir().mkpath(QFileInfo(legacy).absolutePath()));
    for (const QString& target : {QStringLiteral("missing.conf"), legacy}) {
        QFile::remove(legacy);
        QVERIFY(QFile::link(target, legacy)); // dangling, then a loop
        const QStringList before = tree(configDir());
        const auto r = settingsimport::importLegacy(legacy, current);
        QCOMPARE(r.outcome, Outcome::Failed);
        QVERIFY2(r.message.contains(legacy), qPrintable(r.message));
        QCOMPARE(tree(configDir()), before);
    }

    // A link to a settings file is followed; the file is only read.
    QFile::remove(legacy);
    const QString real = g_root->filePath(QStringLiteral("real.conf"));
    QVERIFY(writeFile(real, kLegacy));
    QVERIFY(QFile::link(real, legacy));
    QCOMPARE(settingsimport::importLegacy(legacy, current).outcome, Outcome::Imported);
    QCOMPARE(parsed(current).size(), kLegacyKeys);
    QCOMPARE(readFile(real), QByteArray(kLegacy));
    QVERIFY(QFileInfo(legacy).isSymLink());
    QFile::remove(real);

    // A file where the legacy folder would be: there is no legacy file.
    clearConfig();
    QVERIFY(writeFile(QFileInfo(legacy).absolutePath(), "not a folder"));
    QCOMPARE(settingsimport::importLegacy(legacy, current).outcome, Outcome::NoLegacySettings);
    QVERIFY(!QFileInfo::exists(current));
}

void TestSettingsImport::explicitSettingsFileNeverImports()
{
    QVERIFY(writeFile(settingsimport::legacyFile(), kLegacy));
    const QString own = g_root->filePath(QStringLiteral("own.ini"));
    QFile::remove(own);
    {
        MainWindowOptions o;
        o.startCollector = false;
        o.settingsFile = own;
        MainWindow w(o);
        QCOMPARE(w.intervalMs(), 1000); // not the legacy 5000
        w.setIntervalMs(2000);
        w.close();
    }
    QCOMPARE(parsed(own).value(QStringLiteral("intervalMs")).toInt(), 2000);
    QCOMPARE(parsed(own).value(QStringLiteral("disk")).toString(), QString());
    // Neither default location was used: only the legacy file is there.
    QCOMPARE(tree(configDir()).size(), 2);
    QCOMPARE(readFile(settingsimport::legacyFile()), QByteArray(kLegacy));

    // Without a settings file the window uses bex's own but still imports
    // nothing: only the app's start-up does.
    {
        MainWindowOptions o;
        o.startCollector = false;
        MainWindow w(o);
        QCOMPARE(w.intervalMs(), 1000);
        w.close();
    }
    QVERIFY(QFileInfo::exists(settingsimport::defaultFile()));
    QCOMPARE(parsed(settingsimport::defaultFile()).value(QStringLiteral("disk")).toString(), QString());
    QCOMPARE(readFile(settingsimport::legacyFile()), QByteArray(kLegacy));
}

void TestSettingsImport::appIdentityAndCliTouchNoSettings()
{
    if (qEnvironmentVariableIsEmpty("BEX_APP"))
        QSKIP("bex binary not found (set BEX_APP)");
    QVERIFY(writeFile(settingsimport::legacyFile(), kLegacy));
    const QStringList before = tree(configDir());

    const AppRun version = runApp({QStringLiteral("--version")});
    QCOMPARE(version.exitCode, 0);
    QCOMPARE(version.out.trimmed(), QStringLiteral("bex " BEX_VERSION));

    const AppRun help = runApp({QStringLiteral("--help")});
    QCOMPARE(help.exitCode, 0);
    QVERIFY2(help.out.contains(QStringLiteral("Native Linux performance monitor")), qPrintable(help.out));
    // The usage line repeats the path of the binary, whose folder may be
    // named anything.
    const QString helpText = QString(help.out).replace(qEnvironmentVariable("BEX_APP"), QStringLiteral("<bex>"));
    QVERIFY2(!helpText.contains(QStringLiteral("pulsescope"), Qt::CaseInsensitive), qPrintable(helpText));

    const AppRun theme = runApp({QStringLiteral("--theme-info")});
    QCOMPARE(theme.exitCode, 0);
    QVERIFY2(theme.out.contains(QStringLiteral("style: ")), qPrintable(theme.out));

    for (const QStringList& bad : {QStringList {QStringLiteral("--interval"), QStringLiteral("3")},
                                   QStringList {QStringLiteral("--page"), QStringLiteral("nowhere")}}) {
        const AppRun r = runApp(bad);
        QCOMPARE(r.exitCode, 2);
        QVERIFY2(r.err.startsWith(QStringLiteral("bex: ")), qPrintable(r.err));
    }
    for (const AppRun* r : {&version, &help, &theme})
        QVERIFY2(!r->err.contains(QStringLiteral("PulseScope")), qPrintable(r->err));
    QCOMPARE(tree(configDir()), before);
}

void TestSettingsImport::appImportsOnceAtStartup()
{
    if (qEnvironmentVariableIsEmpty("BEX_APP"))
        QSKIP("bex binary not found (set BEX_APP)");
    const QString legacy = settingsimport::legacyFile();
    const QString current = settingsimport::defaultFile();
    QVERIFY(writeFile(legacy, kLegacy));

    const AppRun first = runApp({QStringLiteral("--quit-after"), QStringLiteral("0.5")});
    QCOMPARE(first.exitCode, 0);
    QCOMPARE(first.err.count(QStringLiteral("bex: Imported 12 settings from PulseScope")), 1);
    // The window read the imported values and saved them again on close.
    const QVariantMap saved = parsed(current);
    QCOMPARE(saved.value(QStringLiteral("intervalMs")).toInt(), 5000);
    QCOMPARE(saved.value(QStringLiteral("windowSeconds")).toInt(), 900);
    QCOMPARE(saved.value(QStringLiteral("details")).toBool(), true);
    QCOMPARE(saved.value(QStringLiteral("smoothGraphs")).toBool(), false);
    QCOMPARE(saved.value(QStringLiteral("disk")).toString(), QStringLiteral("legacydisk0"));
    QCOMPARE(saved.value(QStringLiteral("interface")).toString(), QStringLiteral("legacynet0"));
    QCOMPARE(readFile(legacy), QByteArray(kLegacy));

    // A choice made since then survives the next start: no second import.
    QVERIFY(writeFile(current, "[General]\nintervalMs=2000\n"));
    const AppRun second = runApp({QStringLiteral("--quit-after"), QStringLiteral("0.5")});
    QCOMPARE(second.exitCode, 0);
    QVERIFY2(!second.err.contains(QStringLiteral("PulseScope")), qPrintable(second.err));
    QCOMPARE(parsed(current).value(QStringLiteral("intervalMs")).toInt(), 2000);
    QCOMPARE(parsed(current).value(QStringLiteral("disk")).toString(), QString());
    QCOMPARE(readFile(legacy), QByteArray(kLegacy));
}

void TestSettingsImport::appReportsFailedImport()
{
    if (qEnvironmentVariableIsEmpty("BEX_APP"))
        QSKIP("bex binary not found (set BEX_APP)");
    const QByteArray damaged("[General]\nintervalMs=5000\nwords without equals\n");
    QVERIFY(writeFile(settingsimport::legacyFile(), damaged));
    const AppRun r = runApp({QStringLiteral("--quit-after"), QStringLiteral("0.5")});
    QCOMPARE(r.exitCode, 0);
    QCOMPARE(r.err.count(QStringLiteral("bex: Could not import the PulseScope settings from ") + settingsimport::legacyFile()), 1);
    // bex ran on its defaults and saved them as its own.
    QCOMPARE(parsed(settingsimport::defaultFile()).value(QStringLiteral("intervalMs")).toInt(), 1000);
    QCOMPARE(readFile(settingsimport::legacyFile()), damaged);
}

int main(int argc, char** argv)
{
    // Private HOME and configuration folder before anything reads them;
    // QSettings fixes its user folder on first use.
    g_root = std::make_unique<QTemporaryDir>();
    if (!g_root->isValid()) {
        std::fprintf(stderr, "cannot create a temporary folder\n");
        return 1;
    }
    QDir().mkpath(g_root->filePath(QStringLiteral("home")));
    qputenv("HOME", QFile::encodeName(g_root->filePath(QStringLiteral("home"))));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(configDir()));
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    int status = 0;
    {
        QApplication app(argc, argv);
        QCoreApplication::setOrganizationName(QStringLiteral("bex"));
        QCoreApplication::setApplicationName(QStringLiteral("bex"));
        TestSettingsImport tc;
        QTEST_SET_MAIN_SOURCE_PATH
        status = QTest::qExec(&tc, argc, argv);
    }
    g_root.reset();
    return status;
}

#include "tst_settings_import.moc"
