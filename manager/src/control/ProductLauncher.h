#pragma once

#include "LaunchPlan.h"

#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>

namespace Ucal::Manager {

QStringList applicationArguments(const LaunchPlan& plan);
QString preferredInstalledAdapter(const SoftwareInstallation& software);
LaunchPlan launchPlanFromFrontendJson(const QJsonObject& object);

struct ProductLaunchSpec {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QProcessEnvironment environment;
    QStringList evidenceFiles;
    QString error;
    int resourceSlot = 0;

    bool valid() const { return error.isEmpty() && !program.isEmpty(); }
};
QJsonObject productLaunchSpecToJson(const ProductLaunchSpec& spec);
ProductLaunchSpec productLaunchSpecFromJson(const QJsonObject& object);


class ProductLauncher {
public:
    static ProductLaunchSpec prepare(const LaunchPlan& plan,
                                     const QString& applicationDirectory,
                                     const QString& temporaryDirectory,
                                     UiLanguage language = UiLanguage::English,
                                     const QSet<int>& reservedResourceSlots = {});
};

}  // namespace Ucal::Manager
