#include "UserPreferences.h"

#include "UiPresentation.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace Ucal::Manager {
namespace {

QString quoted(const QString& value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

}  // namespace

UserPreferences loadUserPreferences() {
    QSettings settings(uiLanguagePreferencePath(), QSettings::IniFormat);
    UserPreferences preferences;
    preferences.autoStart = settings.value(QStringLiteral("startup/autoStart"), false).toBool();
    preferences.startMinimized =
        settings.value(QStringLiteral("startup/startMinimized"), false).toBool();
    preferences.autoScan = settings.value(QStringLiteral("startup/autoScan"), true).toBool();
    const int closeBehavior = settings.value(
        QStringLiteral("window/closeBehavior"),
        static_cast<int>(CloseBehavior::HideToTray)).toInt();
    if (closeBehavior >= static_cast<int>(CloseBehavior::HideToTray) &&
        closeBehavior <= static_cast<int>(CloseBehavior::Exit)) {
        preferences.closeBehavior = static_cast<CloseBehavior>(closeBehavior);
    }
    const auto disabled = settings.value(QStringLiteral("backends/disabledIds")).toStringList();
    preferences.disabledBackendIds = QSet<QString>(disabled.cbegin(), disabled.cend());
    return preferences;
}

bool saveUserPreferences(const UserPreferences& preferences) {
    QSettings settings(uiLanguagePreferencePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("startup/autoStart"), preferences.autoStart);
    settings.setValue(QStringLiteral("startup/startMinimized"), preferences.startMinimized);
    settings.setValue(QStringLiteral("startup/autoScan"), preferences.autoScan);
    settings.setValue(QStringLiteral("window/closeBehavior"),
                      static_cast<int>(preferences.closeBehavior));
    QStringList disabled = preferences.disabledBackendIds.values();
    disabled.sort();
    settings.setValue(QStringLiteral("backends/disabledIds"), disabled);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool shouldStartMinimized(const UserPreferences& preferences, const bool productionMode,
                          const bool screenshotMode, const bool trayAvailable,
                          const bool commandLineRequested) {
    return productionMode && !screenshotMode && trayAvailable &&
        (commandLineRequested || preferences.startMinimized);
}

bool configureWindowsAutoStart(const bool enabled, const QString& executablePath,
                               const bool startMinimized, QString* error) {
    const QFileInfo executable(executablePath);
    if (enabled && !executable.isFile()) {
        if (error != nullptr) *error = QStringLiteral("UCAL Manager executable is unavailable.");
        return false;
    }
    QSettings runKey(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
    const QString valueName = QStringLiteral("Ucal Manager");
    if (enabled) {
        QString command = quoted(QDir::toNativeSeparators(executable.absoluteFilePath()));
        if (startMinimized) command += QStringLiteral(" --start-minimized");
        runKey.setValue(valueName, command);
    } else {
        runKey.remove(valueName);
    }
    runKey.sync();
    if (runKey.status() != QSettings::NoError) {
        if (error != nullptr) {
            *error = enabled ? QStringLiteral("Could not create the current-user sign-in entry.")
                             : QStringLiteral("Could not remove the current-user sign-in entry.");
        }
        return false;
    }
    return true;
}

}  // namespace Ucal::Manager
