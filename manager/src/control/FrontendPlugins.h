#pragma once
#include "PackageTool.h"
#include <QJsonObject>
#include <QSet>
#include <QVector>
#include <memory>

namespace Ucal::Manager {
struct FrontendPackage {
    QString id, name, version, manifestPath, implementation, libraryPath;
    QSet<QString> operations;
    int maximumCanChannels = 16;
    bool fullAccess = false;
    PackageTool configurationTool;
    QJsonValue description;
    QJsonObject integrations;
    QJsonValue compatibilityReady;
    QJsonValue compatibilityUnavailable;
    QJsonObject configurationFile;
    bool refreshIntegration = false;
};
struct FrontendResult {
    QJsonObject value;
    QString error;
    int code = 0;
    bool ok() const { return code == 0 && error.isEmpty(); }
};
class Frontend {
  public:
    virtual ~Frontend() = default;
    virtual FrontendResult scan();
    virtual FrontendResult inspect(const QJsonObject &context);
    virtual FrontendResult checkCompatibility(const QJsonObject &context);
    virtual FrontendResult prepareLaunch(const QJsonObject &context);
    virtual FrontendResult deploy(const QJsonObject &context);
    virtual FrontendResult restoreDeployment(const QJsonObject &context);

  protected:
    virtual FrontendResult invoke(unsigned operation, const QJsonObject &context);
};
struct FrontendDirectoryScan {
    QVector<FrontendPackage> packages;
    QStringList diagnostics;
};
QString frontendDirectory(const QString &applicationDirectory);
FrontendDirectoryScan scanFrontendDirectory(const QString &directory);
std::shared_ptr<Frontend> createFrontend(const FrontendPackage &package);
FrontendPackage frontendPackage(const QString &id);
FrontendResult invokeFrontend(const QString &id, const QString &operation, const QJsonObject &context = {});
} // namespace Ucal::Manager
