#include "SoftwareDiscovery.h"
#include "FrontendPlugins.h"
#include "PortablePaths.h"
#include <QSaveFile>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QJsonArray>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSettings>

#include <windows.h>
#include <winver.h>

#include <vector>

namespace Ucal::Manager {
namespace {
QString searchPathFile() { return QDir(portableDataDirectory()).filePath(QStringLiteral("software-search-paths.json")); }
QStringList knownSoftwarePaths() {
    QFile file(searchPathFile());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return {};
    QStringList result;
    for (const auto& value : QJsonDocument::fromJson(file.readAll()).array())
        if (value.isString() && QFileInfo(value.toString()).isAbsolute() && !result.contains(value.toString())) result << value.toString();
    return result.mid(0, 256);
}
}
void rememberSoftwareSearchPath(const QString& path) {
    const auto normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    auto paths = knownSoftwarePaths();
    if (paths.contains(normalized, Qt::CaseInsensitive)) return;
    paths.prepend(normalized); paths = paths.mid(0,256);
    QDir().mkpath(portableDataDirectory());
    QSaveFile file(searchPathFile());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonArray::fromStringList(paths)).toJson()); file.commit();
    }
}
namespace {
SoftwareInstallation packageInstallation(const FrontendPackage& package, const QJsonObject& object) {
    const auto path = object.value("executablePath").toString();
    if (!QFileInfo(path).isFile()) return {};
    SoftwareInstallation s;
    s.id = stableSoftwareId(package.id, path);
    s.family = package.id;
    s.displayName = object.value("displayName").toString(package.name);
    s.version = object.value("version").toString();
    if (s.version.isEmpty()) s.version = executableVersion(path);
    s.architecture = executableArchitecture(path);
    s.executablePath = QFileInfo(path).absoluteFilePath();
    s.source = object.value("source").toString(QStringLiteral("Frontend package"));
    s.compatibility = object.value("compatibility").toString();
    s.compatibilityKind = object.value("compatibilityKind").toString(QStringLiteral("warning"));
    for (const auto& a : object.value("integrationAdapters").toArray())
        if (a.isString() && !a.toString().isEmpty()) s.integrationAdapters << a.toString();
    return s;
}
}

SoftwareInstallation inspectSoftwareExecutable(const QString& path, const QString& source, QString* error) {
    if (error) error->clear();
    QStringList failures;
    const auto scan = scanFrontendDirectory(frontendDirectory(QCoreApplication::applicationDirPath()));
    for (const auto& p : scan.packages) {
        if (!p.operations.contains(QStringLiteral("inspect"))) continue;
        const auto result = createFrontend(p)->inspect({{"executablePath", path}, {"source", source}});
        if (!result.ok()) { failures << p.name + QStringLiteral(": ") + result.error; continue; }
        auto s = packageInstallation(p, result.value);
        if (!s.id.isEmpty() && QDir::cleanPath(s.executablePath).compare(
                QDir::cleanPath(QFileInfo(path).absoluteFilePath()), Qt::CaseInsensitive) == 0) {
            s.source = source;
            return s;
        }
    }
    if (!failures.isEmpty()) {
        if (error) *error = failures.join(QLatin1Char('\n'));
        return {};
    }
    if (!QFileInfo(path).isFile()) return {};
    SoftwareInstallation s;
    s.id = stableSoftwareId(QStringLiteral("custom"), path);
    s.family = QStringLiteral("custom");
    s.displayName = QFileInfo(path).completeBaseName();
    s.executablePath = QFileInfo(path).absoluteFilePath();
    s.version = executableVersion(path); s.architecture = executableArchitecture(path);
    s.source = source; s.compatibilityKind = QStringLiteral("danger");
    s.compatibility = QStringLiteral("Unsupported: no frontend plugin recognizes this executable");
    return s;
}

QVector<SoftwareInstallation> discoverInstalledSoftware(QStringList* diagnostics) {
    const auto scan = scanFrontendDirectory(frontendDirectory(QCoreApplication::applicationDirPath()));
    if (diagnostics) *diagnostics = scan.diagnostics;
    QMap<QString, SoftwareInstallation> unique;
    for (const auto& p : scan.packages) {
        if (!p.operations.contains(QStringLiteral("scan"))) continue;
        const auto result = createFrontend(p)->scan();
        if (!result.ok()) {
            if (diagnostics) *diagnostics << p.name + QStringLiteral(": ") + result.error;
            continue;
        }
        if (!result.value.value("installations").isArray()) {
            if (diagnostics) *diagnostics << p.name + QStringLiteral(": Invalid scan result");
            continue;
        }
        for (const auto& item : result.value.value("installations").toArray()) {
            const auto s = packageInstallation(p, item.toObject());
            if (!s.id.isEmpty()) unique.insert(s.id, s);
        }
    }
    // Reinspect explicitly added locations, including portable installations not registered with Windows.
    for (const auto& path : knownSoftwarePaths()) {
        if (!QFileInfo(path).isFile()) continue;
        for (const auto& p : scan.packages) {
            if (!p.operations.contains(QStringLiteral("inspect"))) continue;
            const auto result = createFrontend(p)->inspect({{"executablePath", path}, {"source", "Manual"}});
            if (!result.ok()) { if (diagnostics) *diagnostics << p.name + ": " + result.error; continue; }
            const auto s = packageInstallation(p, result.value);
            if (!s.id.isEmpty()) { unique.insert(s.id, s); break; }
        }
    }
    return unique.values().toVector();
}

}  // namespace Ucal::Manager
