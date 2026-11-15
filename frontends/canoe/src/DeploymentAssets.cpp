#include "DeploymentAssets.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
namespace Ucal::Manager {
namespace {
QString safeApplicationId(QString id) {
 id.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
 while(id.startsWith(QLatin1Char('.'))) id[0]=QLatin1Char('_');
 return id.isEmpty()?QStringLiteral("application"):id.left(96);
}
}
VxlProductAssets resolveVxlProductAssets(const QString& productRoot,
                                         const QString& machine) {
    VxlProductAssets assets;
    assets.scriptPath = resolveVxlDeploymentScript(productRoot);
    QString architectureDirectory;
    QString frontendName;
    QString discoveryName;
    if (machine == QStringLiteral("x86")) {
        architectureDirectory = QStringLiteral("x86");
        frontendName = QStringLiteral("vxlapi.dll");
        discoveryName = QStringLiteral("UcalXlDiscovery.dll");
    } else if (machine == QStringLiteral("x64")) {
        architectureDirectory = QStringLiteral("x64");
        frontendName = QStringLiteral("vxlapi64.dll");
        discoveryName = QStringLiteral("UcalXlDiscovery64.dll");
    } else {
        assets.error = QStringLiteral("Only x86 and x64 VXL clients are supported.");
        return assets;
    }
    QString runtime = QDir(productRoot).filePath(
        QStringLiteral("frontends/vxlapi/%1").arg(architectureDirectory));
    if (!QFileInfo(QDir(runtime).filePath(frontendName)).isFile())
        runtime = QDir(productRoot).filePath(QStringLiteral("runtime/%1/frontends").arg(architectureDirectory));
    assets.frontendPath = QDir(runtime).filePath(frontendName);
    assets.discoveryPath = QDir(runtime).filePath(discoveryName);
    if (!QFileInfo(assets.scriptPath).isFile() || !QFileInfo(assets.frontendPath).isFile() ||
        !QFileInfo(assets.discoveryPath).isFile()) {
        assets.error = QStringLiteral("The product copy does not contain the matching deployment helper and frontend assets.");
    }
    return assets;
}

QString resolveVxlDeploymentScript(const QString& productRoot) {
    const QString scriptOverride = qEnvironmentVariable("UCAL_MANAGER_VXL_DEPLOYMENT_SCRIPT");
    if (!scriptOverride.isEmpty()) return QFileInfo(scriptOverride).absoluteFilePath();
    for (const auto& relative : {
            QStringLiteral("frontends/vxlapi/tools/Set-UcalVxlClientDeployment.ps1"),
            QStringLiteral("frontends/canoe/tools/vxl/Set-UcalVxlClientDeployment.ps1"),
            QStringLiteral("tools/vxl-client/Set-UcalVxlClientDeployment.ps1"),
            QStringLiteral("frontends/vxlapi/tools/Set-UcalVxlClientDeployment.ps1")}) {
        const auto path = QDir(productRoot).filePath(relative);
        if (QFileInfo(path).isFile()) return path;
    }
    return QDir(productRoot).filePath(QStringLiteral("tools/vxl-client/Set-UcalVxlClientDeployment.ps1"));
}

QString latestVxlDeploymentBackup(const QString& workspacePath,
                                  const QString& applicationId) {
    const QDir root(QDir(QFileInfo(workspacePath).absolutePath())
                        .filePath(QStringLiteral("deployments/%1")
                                      .arg(safeApplicationId(applicationId))));
    const QFileInfoList entries = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo& entry : entries) {
        if (QFileInfo(QDir(entry.absoluteFilePath())
                          .filePath(QStringLiteral("deployment.json"))).isFile()) {
            return entry.absoluteFilePath();
        }
    }
    return {};
}

QString nextVxlDeploymentBackup(const QString& workspacePath,
                                const QString& applicationId) {
    const QString root = QDir(QFileInfo(workspacePath).absolutePath())
                             .filePath(QStringLiteral("deployments/%1")
                                           .arg(safeApplicationId(applicationId)));
    return QDir(root).filePath(
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ")));
}

}
