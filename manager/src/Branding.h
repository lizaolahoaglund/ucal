#pragma once

#include <QIcon>
#include <QImage>
#include <QString>

namespace Ucal::Manager {

QIcon managerIcon();
QImage composeApplicationIcon(const QImage& original, const QImage& badge, int size);
bool writeIconFile(const QString& path, const QIcon& icon, QString* error = nullptr);
QString createShortcutIcon(const QString& softwareExecutable, const QString& directory,
                             QString* error = nullptr);

} // namespace Ucal::Manager
