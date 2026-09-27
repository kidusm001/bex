#include "app/cli.h"

#include "app/mainwindow.h"

#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QStringList>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

namespace cli {

namespace {

std::optional<double> parseSeconds(const QString& text)
{
    bool ok = false;
    const double v = text.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(v))
        return std::nullopt;
    return v;
}

} // namespace

std::optional<int> parseIntervalMs(const QString& text)
{
    const auto seconds = parseSeconds(text);
    if (!seconds)
        return std::nullopt;
    // Compare in floating point; only a matching choice is ever returned.
    for (int choice : MainWindow::intervalChoicesMs())
        if (std::fabs(*seconds * 1000.0 - choice) < 1e-6)
            return choice;
    return std::nullopt;
}

std::optional<int> parseDelayMs(const QString& text, double maxSeconds)
{
    const auto seconds = parseSeconds(text);
    if (!seconds || *seconds < 0 || !(maxSeconds >= 0) || maxSeconds > kMaxDelaySeconds || *seconds > maxSeconds)
        return std::nullopt;
    return static_cast<int>(std::llround(*seconds * 1000.0)); // at most 86 400 000
}

std::optional<QSize> parseWindowSize(const QString& text)
{
    const QStringList parts = text.trimmed().toLower().split(QLatin1Char('x'));
    if (parts.size() != 2)
        return std::nullopt;
    int side[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const QString& p = parts[i];
        // Plain digits only (no sign, spaces or exponent), short enough that
        // the conversion cannot overflow.
        if (p.isEmpty() || p.size() > 5)
            return std::nullopt;
        for (QChar c : p)
            if (c < QLatin1Char('0') || c > QLatin1Char('9'))
                return std::nullopt;
        side[i] = p.toInt();
        if (side[i] < kMinWindowSide || side[i] > kMaxWindowSide)
            return std::nullopt;
    }
    return QSize(side[0], side[1]);
}

namespace {

struct ThemeArguments {
    bool seen = false;            // any -platformtheme
    std::optional<QString> value; // the name Qt takes, possibly empty
    bool operator==(const ThemeArguments&) const = default;
};

// The command line as QGuiApplication::createPlatformIntegration() reads it
// (Qt 6): options taking a value consume the next argument, even one that
// looks like an option, and the last -platformtheme wins over
// QT_QPA_PLATFORMTHEME. -title, -geometry and -icon take a value only while
// the platform plugin name (from the start or a -platform before them)
// begins with "xcb". A joined "-platformtheme=name" is not read by Qt, but
// still counts as an explicit wish not to override.
ThemeArguments readThemeArguments(int argc, const char* const* argv, QByteArray platform)
{
    ThemeArguments r;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!a || a[0] != '-')
            continue;
        const bool xcb = platform.startsWith("xcb");
        if (a[1] == '-')
            ++a;
        auto is = [a](const char* option) { return std::strcmp(a, option) == 0; };
        if (is("-platformtheme")) {
            r.seen = true;
            if (++i < argc)
                r.value = QString::fromLocal8Bit(argv[i]);
        } else if (is("-platform")) {
            if (++i < argc)
                platform = QByteArray(argv[i]);
        } else if (is("-platformpluginpath") || is("-qwindowgeometry") || is("-qwindowtitle") || is("-qwindowicon")
                   || (xcb && (is("-geometry") || is("-title") || is("-icon")))) {
            ++i;
        } else if (std::strncmp(a, "-platformtheme=", 15) == 0) {
            r.seen = true;
        }
    }
    return r;
}

// The platform plugin name Qt starts from, as far as it is known before
// start-up: QT_QPA_PLATFORM, else Wayland or X11 from the session. Without
// either, Qt uses its built-in default, which is not known here.
std::optional<QByteArray> startPlatform()
{
    const QByteArray chosen = qgetenv("QT_QPA_PLATFORM");
    if (!chosen.isEmpty())
        return chosen;
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY") || qgetenv("XDG_SESSION_TYPE") == "wayland")
        return QByteArray("wayland");
    if (qEnvironmentVariableIsSet("DISPLAY"))
        return QByteArray("xcb");
    return std::nullopt;
}

} // namespace

ThemeEnvironment probeThemeEnvironment(int argc, const char* const* argv)
{
    ThemeEnvironment env;
    env.platformThemeVariable = qEnvironmentVariable("QT_QPA_PLATFORMTHEME");
    env.currentDesktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    // With the platform unknown, read the arguments both ways; they are
    // uncertain only when the two readings differ.
    const std::optional<QByteArray> platform = startPlatform();
    const ThemeArguments args = readThemeArguments(argc, argv, platform.value_or(QByteArray()));
    if (!platform && !(readThemeArguments(argc, argv, QByteArray("xcb")) == args)) {
        env.themeArgument = true;
        env.themeArgumentUncertain = true;
    } else {
        env.themeArgument = args.seen;
        env.themeArgumentValue = args.value.value_or(QString());
        env.themeArgumentCleared = args.value && args.value->isEmpty();
    }

    QString configHome = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (configHome.isEmpty() || !QDir::isAbsolutePath(configHome))
        configHome = QDir::homePath() + QStringLiteral("/.config");
    const QString config = configHome + QStringLiteral("/qt6ct/qt6ct.conf");
    if (QFileInfo(config).isFile())
        env.qt6ctConfig = config;

    QStringList dirs = qEnvironmentVariable("QT_PLUGIN_PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts);
    dirs << QLibraryInfo::path(QLibraryInfo::PluginsPath);
    for (const QString& dir : std::as_const(dirs)) {
        const QString plugin = dir + QStringLiteral("/platformthemes/libqt6ct.so");
        if (QFileInfo(plugin).isFile()) {
            env.qt6ctPlugin = plugin;
            break;
        }
    }
    return env;
}

ThemeDecision chooseTheme(const ThemeEnvironment& env)
{
    ThemeDecision d;
    if (env.themeArgumentUncertain) {
        d.note = QStringLiteral("Not known whether the command line chooses the platform theme: Qt reads -title, "
                                "-geometry and -icon as taking a value only on the xcb platform, and the platform is "
                                "not known before start-up (QT_QPA_PLATFORM, DISPLAY and WAYLAND_DISPLAY are unset). "
                                "Qt's own choice is kept.");
        return d;
    }
    // Qt reads the variable first and lets the command line override it.
    if (!env.themeArgumentValue.isEmpty()) {
        d.note = QStringLiteral("Platform theme from the -platformtheme argument (%1).").arg(env.themeArgumentValue);
        if (!env.platformThemeVariable.isEmpty() && env.platformThemeVariable != env.themeArgumentValue)
            d.note += QStringLiteral(" It overrides QT_QPA_PLATFORMTHEME (%1).").arg(env.platformThemeVariable);
        return d;
    }
    if (env.themeArgumentCleared) {
        d.note = env.platformThemeVariable.isEmpty()
            ? QStringLiteral("The -platformtheme argument is empty, so Qt chooses the platform theme itself.")
            : QStringLiteral("The -platformtheme argument is empty, so Qt ignores QT_QPA_PLATFORMTHEME (%1) and "
                             "chooses the platform theme itself.")
                  .arg(env.platformThemeVariable);
        return d;
    }
    if (!env.platformThemeVariable.isEmpty()) {
        d.note = QStringLiteral("Platform theme from QT_QPA_PLATFORMTHEME (%1).").arg(env.platformThemeVariable);
        return d;
    }
    if (env.themeArgument) {
        d.note = QStringLiteral("A -platformtheme argument without a separate theme name was given; Qt does not "
                                "read it, so Qt chooses the platform theme itself.");
        return d;
    }
    // Desktops whose Qt integration Qt selects by itself.
    static const QStringList integrated {
        QStringLiteral("KDE"),      QStringLiteral("GNOME"),   QStringLiteral("UNITY"),  QStringLiteral("X-CINNAMON"),
        QStringLiteral("CINNAMON"), QStringLiteral("MATE"),    QStringLiteral("XFCE"),   QStringLiteral("LXDE"),
        QStringLiteral("LXQT"),     QStringLiteral("PANTHEON"), QStringLiteral("BUDGIE"), QStringLiteral("DEEPIN"),
        QStringLiteral("UKUI")};
    const QStringList desktops = env.currentDesktop.toUpper().split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString& desktop : desktops) {
        if (integrated.contains(desktop)) {
            d.note = QStringLiteral("Platform theme chosen by Qt for the %1 desktop.").arg(env.currentDesktop);
            return d;
        }
    }
    if (env.qt6ctConfig.isEmpty()) {
        d.note = QStringLiteral("Qt default platform theme (no qt6ct configuration and QT_QPA_PLATFORMTHEME unset).");
        return d;
    }
    if (env.qt6ctPlugin.isEmpty()) {
        d.pluginMissing = true;
        d.note = QStringLiteral("qt6ct is configured (%1) but its Qt 6 plugin, platformthemes/libqt6ct.so, is not "
                                "installed, so Qt's default look is used.")
                     .arg(QDir::toNativeSeparators(env.qt6ctConfig));
        return d;
    }
    d.requestQt6ct = true;
    d.note = QStringLiteral("Platform theme qt6ct, as configured in %1 (QT_QPA_PLATFORMTHEME is unset in this "
                            "session; set it or pass -platformtheme to choose another).")
                 .arg(QDir::toNativeSeparators(env.qt6ctConfig));
    return d;
}

} // namespace cli
