#pragma once

#include <QString>
#include <QSet>

namespace Ucal::Manager {

enum class CloseBehavior {
    HideToTray = 0,
    Ask = 1,
    Exit = 2,
};

struct UserPreferences {
    bool autoStart = false;
    bool startMinimized = false;
    bool autoScan = true;
    CloseBehavior closeBehavior = CloseBehavior::HideToTray;
    QSet<QString> disabledBackendIds;
};

UserPreferences loadUserPreferences();
bool saveUserPreferences(const UserPreferences& preferences);

// Keeps the startup visibility policy independently testable from QApplication/main.
bool shouldStartMinimized(const UserPreferences& preferences, bool productionMode,
                          bool screenshotMode, bool trayAvailable,
                          bool commandLineRequested);

// Updates only the current user's Windows sign-in launch entry. The operation is
// reversible and never writes a machine-wide key.
bool configureWindowsAutoStart(bool enabled, const QString& executablePath,
                               bool startMinimized, QString* error = nullptr);

}  // namespace Ucal::Manager
