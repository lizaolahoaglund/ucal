#pragma once

#include "ManagerModel.h"

#include <QStringList>
#include <QVector>

namespace Ucal::Manager {
void rememberSoftwareSearchPath(const QString& path);
QString executableVersion(const QString& path);
QString executableArchitecture(const QString& path);
QString stableSoftwareId(const QString& family, const QString& path);

QVector<SoftwareInstallation> discoverInstalledSoftware(QStringList* diagnostics = nullptr);
SoftwareInstallation inspectSoftwareExecutable(
    const QString& executablePath,
    const QString& source = QStringLiteral("Manual"), QString* error = nullptr);


}  // namespace Ucal::Manager
