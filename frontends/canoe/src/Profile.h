#pragma once
#include "FrontendPlugins.h"
#include "SoftwareDiscovery.h"
#include "ProductLauncher.h"
namespace Ucal::Manager {
QStringList canoeIntegrationAdapters(const QString& version, const QString& architecture);
FrontendResult deployPublicContract(const QJsonObject& context, bool restore);
std::shared_ptr<Frontend> createProfileFrontend(const FrontendPackage&);
QVector<SoftwareInstallation> discoverProfileInstallations(const QString& family);
SoftwareInstallation inspectProfileExecutable(const QString& path, const QString& source, const QString& family);
QString preferredProfileAdapter(const SoftwareInstallation&);
ProductLaunchSpec prepareProfileLaunch(const QString& implementation, const LaunchPlan&, const QString& applicationDirectory,
    const QString& temporaryDirectory, UiLanguage, const QSet<int>& reservedResourceSlots);
bool synchronizeCanoeLaunchAssets(const LaunchPlan&, const QString& applicationDirectory,
    UiLanguage, ProductLaunchSpec*);
}
