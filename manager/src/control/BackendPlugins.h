#pragma once
#include "ManagerModel.h"
#include <QJsonObject>
namespace Ucal::Manager {
bool parseBackendPlugin(const QJsonObject &object, const QString &manifest, BackendPackage &package,
                        QString &error);
QString backendPluginSettingsDirectory();
QString backendPluginCatalogPath();
QVector<BackendChannelBinding> loadBackendPluginChannels(const BackendPackage &package,
                                                         QString *error = nullptr);
bool saveBackendPluginChannels(const BackendPackage &package, const QVector<BackendChannelBinding> &channels,
                               QString *error = nullptr);
// Resolves shared adapter/worker, atomically updates per-instance settings/catalog.
QString synchronizeBackendPlugins(QVector<BackendPackage> &packages, const QString &applicationDirectory,
                                  const QString &settingsDirectory = QString());
} // namespace Ucal::Manager
