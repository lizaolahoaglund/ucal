#pragma once
#include <QString>
namespace Ucal::Manager {
struct VxlProductAssets {
    QString scriptPath;
    QString frontendPath;
    QString discoveryPath;
    QString error;
    bool ready() const {
        return error.isEmpty() && !scriptPath.isEmpty() && !frontendPath.isEmpty() &&
               !discoveryPath.isEmpty();
    }
};

QString resolveVxlDeploymentScript(const QString& productRoot);
VxlProductAssets resolveVxlProductAssets(const QString& productRoot,
                                         const QString& machine);
QString latestVxlDeploymentBackup(const QString& workspacePath,
                                  const QString& applicationId);
QString nextVxlDeploymentBackup(const QString& workspacePath,
                                const QString& applicationId);

}
