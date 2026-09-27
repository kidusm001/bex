// Command-line value parsing that validates before converting, so huge,
// non-finite or malformed values are rejected instead of reaching an
// undefined floating-point to integer conversion.
#pragma once

#include <QSize>
#include <QString>

#include <optional>

namespace cli {

// A sampling interval in seconds that is exactly one of the supported
// choices (0.5, 1, 2, 5); returns it in milliseconds.
std::optional<int> parseIntervalMs(const QString& text);

// A finite, non-negative number of seconds no larger than maxSeconds,
// rounded to whole milliseconds.
inline constexpr double kMaxDelaySeconds = 86400.0;
std::optional<int> parseDelayMs(const QString& text, double maxSeconds = kMaxDelaySeconds);

// A window size "WIDTHxHEIGHT" in pixels, each from kMinWindowSide to
// kMaxWindowSide.
inline constexpr int kMinWindowSide = 320;
inline constexpr int kMaxWindowSide = 16384;
std::optional<QSize> parseWindowSize(const QString& text);

// Qt platform theme selection, decided before QApplication exists.
//
// Desktops such as KDE or GNOME give Qt their own theme integration, and
// many others set QT_QPA_PLATFORMTHEME in the session. Some (Hyprland, Sway
// and other compositors started from a login shell) leave it unset even
// though the user configured qt6ct, so Qt applications started from a
// launcher fall back to Qt's built-in look. PulseScope then asks for qt6ct
// for itself only, through its own command line; nothing global changes.
struct ThemeEnvironment {
    QString platformThemeVariable; // QT_QPA_PLATFORMTHEME
    QString currentDesktop;        // XDG_CURRENT_DESKTOP
    bool themeArgument = false;    // -platformtheme on the command line
    // The theme name Qt takes from the command line (the argument after the
    // last -platformtheme); empty when there is none. It overrides the
    // variable, as in Qt.
    QString themeArgumentValue;
    // That argument is present but empty ("-platformtheme ''"): Qt then
    // ignores QT_QPA_PLATFORMTHEME and chooses the theme itself.
    bool themeArgumentCleared = false;
    // Qt's reading of the command line depends on its platform plugin (on
    // xcb, -title, -geometry and -icon take a value), and the plugin is not
    // known before start-up: whether the arguments choose a theme is unknown.
    bool themeArgumentUncertain = false;
    QString qt6ctConfig;           // qt6ct.conf path, empty when missing
    QString qt6ctPlugin;           // libqt6ct.so path, empty when missing
};

struct ThemeDecision {
    bool requestQt6ct = false; // add "-platformtheme qt6ct" to the arguments
    bool pluginMissing = false; // qt6ct is configured but cannot be loaded
    QString note;               // how the theme was chosen, for About and --theme-info
};

// Reads the environment, the command line, the qt6ct configuration and the
// Qt plugin directories.
ThemeEnvironment probeThemeEnvironment(int argc, const char* const* argv);
// Pure decision: an explicit choice (argument or variable, the argument
// first as in Qt) is always kept, a desktop with its own integration is left
// alone, and qt6ct is requested only when it is configured and its plugin is
// installed.
ThemeDecision chooseTheme(const ThemeEnvironment& env);

} // namespace cli
