#pragma once
#include <QJsonValue>
#include <QStringList>

namespace Ucal::Manager {
struct PackageTool {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    bool available() const { return !program.isEmpty(); }
};
bool parsePackageTool(const QJsonValue &value, const QString &manifest, PackageTool &tool, QString &error);
bool startPackageTool(const PackageTool &tool, QString *error = nullptr);
} // namespace Ucal::Manager
