#include "PortablePaths.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
namespace Ucal::Manager {
QString resolveProductRoot(const QString& startDirectory) {
    QDir directory(startDirectory);
    for (int depth = 0; depth < 8; ++depth) {
        // An extracted package is a boundary even when damaged: never silently
        // borrow a developer checkout's executable from a parent directory.
        if (QFileInfo(directory.filePath(QStringLiteral("UcalManagerPackage.json"))).isFile() ||
            (QFileInfo(directory.filePath(QStringLiteral("runtime/x64/UcalBroker.exe"))).isFile() &&
             QDir(directory.filePath(QStringLiteral("backends"))).exists()) ||
            (QFileInfo(directory.filePath(QStringLiteral("Ucal.sln"))).isFile() &&
             QFileInfo(directory.filePath(QStringLiteral("manager/UcalManager.pro"))).isFile()))
            return directory.absolutePath();
        if (!directory.cdUp()) break;
    }
    return {};
}
QString portableDataDirectory() {
    const auto requested = qEnvironmentVariable("UCAL_MANAGER_SETTINGS_DIRECTORY");
    return QFileInfo(requested).isAbsolute() ? QDir::cleanPath(requested)
        : QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
}
bool migrateLegacyState(QString* error) {
    if (error) error->clear();
    if (!qEnvironmentVariableIsEmpty("UCAL_MANAGER_SETTINGS_DIRECTORY")) return true;
    const auto target = portableDataDirectory();
    const auto source = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (QDir::cleanPath(target) == QDir::cleanPath(source) || !QFileInfo(source).isDir()) return true;
    QDirIterator files(source, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        const auto relative = QDir(source).relativeFilePath(path);
        // Session logs and deployed vendor application copies are not configuration.
        if (!(relative == "preferences.ini" || relative == "software-search-paths.json" || relative.startsWith("profiles") ||
              relative.startsWith("backend-plugins/") || relative.startsWith("backends/"))) continue;
        const auto destination = QDir(target).filePath(relative);
        if (QFileInfo::exists(destination)) continue;
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(path, destination)) {
            if (error) *error = QStringLiteral("Cannot copy existing configuration to %1").arg(destination);
            return false;
        }
    }
    return true;
}
}
