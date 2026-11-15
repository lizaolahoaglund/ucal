#pragma once

#include "ManagerModel.h"

#include <QString>

namespace Ucal::Manager {

struct BackendDirectoryScan {
    QVector<BackendPackage> packages;
    QStringList diagnostics;
};

QString backendDirectory(const QString& applicationDirectory);
BackendDirectoryScan scanBackendDirectory(const QString& directory);
QString runtimeFileVersion(const QString& path);
QString runtimeLibraryArchitecture(const QString& path);

}  // namespace Ucal::Manager
