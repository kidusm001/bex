// One-time import of the settings saved while the app was called PulseScope
// into bex's own settings file.
#pragma once

#include <QString>

namespace settingsimport {

enum class Outcome {
    Imported,         // newPath was created holding every legacy setting
    NewSettingsExist, // newPath exists (whatever it holds); nothing was read or written
    NoLegacySettings, // there is no legacy file, or it holds no settings
    Failed,           // nothing was imported; message says why
};

struct Result {
    Outcome outcome = Outcome::NoLegacySettings;
    int keys = 0;    // settings imported
    QString message; // for Imported and Failed: one or two sentences for the user
};

// Where PulseScope kept its settings: PulseScope/PulseScope.conf in the
// user's configuration folder ($XDG_CONFIG_HOME, by default ~/.config).
QString legacyFile();
// The file QSettings() uses under the application's organization and name,
// that is, MainWindow's settings when MainWindowOptions::settingsFile is
// empty (for bex: bex/bex.conf in the same folder).
QString defaultFile();

// The largest legacy file imported; a larger one fails. The settings the
// app writes take a few KiB.
inline constexpr qint64 kMaxLegacyBytes = qint64(8) << 20;

// Copies every setting in legacyPath into a new file at newPath, unless
// newPath already exists. legacyPath is only read. The copy is written
// beside newPath and renamed into place only once complete, never over a
// file that appeared meanwhile, so newPath is either absent or complete.
// A path that cannot be checked (a folder that cannot be searched, say)
// fails rather than counting as absent.
Result importLegacy(const QString& legacyPath, const QString& newPath);

} // namespace settingsimport
