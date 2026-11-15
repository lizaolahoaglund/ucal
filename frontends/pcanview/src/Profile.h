#pragma once
#include "FrontendPlugins.h"
#include "SoftwareDiscovery.h"
#include "ProductLauncher.h"
namespace Ucal::Manager {
std::shared_ptr<Frontend> createProfileFrontend(const FrontendPackage&);
QVector<SoftwareInstallation> discoverProfileInstallations(const QString& family);
SoftwareInstallation inspectProfileExecutable(const QString& path, const QString& source, const QString& family);
QStringList compatiblePcanViewIntegrationAdapters(const QString& version, const QString& architecture);
QString preferredProfileAdapter(const SoftwareInstallation&);
ProductLaunchSpec prepareProfileLaunch(const QString& implementation, const LaunchPlan&, const QString& applicationDirectory,
    const QString& temporaryDirectory, UiLanguage, const QSet<int>& reservedResourceSlots);
}
