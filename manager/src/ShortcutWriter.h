#pragma once

#include <QString>

namespace Ucal::Manager {

QString defaultProfileShortcutPath(const QString& applicationName);
bool createProfileShortcut(const QString& managerExecutable, const QString& applicationId,
                           const QString& applicationName, const QString& shortcutPath,
                           QString* error = nullptr, const QString& workspacePath = QString(),
                           const QString& softwareExecutable = QString());
bool readProfileShortcut(const QString& shortcutPath, QString* target, QString* arguments,
                         QString* error = nullptr, QString* iconPath = nullptr);

}  // namespace Ucal::Manager
