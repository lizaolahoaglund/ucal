#include "CanoeText.h"
#include "Profile.h"
#include "SoftwareDiscovery.h"
#include "FrontendPlugins.h"
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

struct Candidate {
    QString family;
    QString displayName;
    QString version;
    QString executablePath;
    QString source;
};

QString cleanExecutableValue(QString value) {
    value = value.trimmed();
    if (value.startsWith(QLatin1Char('"'))) {
        const int closing = value.indexOf(QLatin1Char('"'), 1);
        if (closing > 1) return value.mid(1, closing - 1);
    }
    const int comma = value.lastIndexOf(QLatin1Char(','));
    if (comma > 2) value = value.left(comma);
    return QDir::fromNativeSeparators(value.trimmed());
}

void appendIfExecutable(QVector<Candidate>* candidates, const Candidate& candidate) {
    if (candidates == nullptr) return;
    Candidate normalized = candidate;
    normalized.executablePath = QFileInfo(cleanExecutableValue(candidate.executablePath))
                                    .absoluteFilePath();
    if (!QFileInfo(normalized.executablePath).isFile()) return;
    candidates->push_back(normalized);
}

void scanUninstallRoot(const QString& root, QVector<Candidate>* candidates) {
    QSettings settings(root, QSettings::NativeFormat);
    for (const QString& group : settings.childGroups()) {
        settings.beginGroup(group);
        const QString name = settings.value(QStringLiteral("DisplayName")).toString().trimmed();
        const QString version = settings.value(QStringLiteral("DisplayVersion")).toString().trimmed();
        const QString location = settings.value(QStringLiteral("InstallLocation")).toString().trimmed();
        const QString displayIcon = settings.value(QStringLiteral("DisplayIcon")).toString().trimmed();
        settings.endGroup();

        if (name.startsWith(QStringLiteral("Vector CANoe "), Qt::CaseInsensitive) &&
            name.contains(QStringLiteral("bit"), Qt::CaseInsensitive)) {
            const QStringList candidatesAtLocation = {
                QDir(location).filePath(QStringLiteral("Exec64/CANoe64.exe")),
                QDir(location).filePath(QStringLiteral("Exec32/CANoe32.exe")),
                cleanExecutableValue(displayIcon),
            };
            for (const QString& executable : candidatesAtLocation) {
                Candidate candidate{QStringLiteral("canoe"), name, version, executable,
                                    QStringLiteral("Windows Registry")};
                const int before = candidates->size();
                appendIfExecutable(candidates, candidate);
                if (candidates->size() != before) break;
            }
        }


    }
}

void scanAppPaths(const QString& root, QVector<Candidate>* candidates) {
    QSettings settings(root, QSettings::NativeFormat);
    for (const QString& group : settings.childGroups()) {
        const QString lower = group.toLower();
        if (lower != QStringLiteral("canoe64.exe") && lower != QStringLiteral("canoe32.exe")) continue;
        settings.beginGroup(group);
        QString executable = settings.value(QStringLiteral("Default")).toString();
        if (executable.isEmpty()) executable = settings.value(QStringLiteral(".")).toString();
        settings.endGroup();
        appendIfExecutable(candidates,
            {QStringLiteral("canoe"),
             QStringLiteral("Vector CANoe"),
             QString(), executable, QStringLiteral("Windows App Paths")});
    }
}

SoftwareInstallation projectCandidate(const Candidate& candidate) {
    SoftwareInstallation software;
    software.family = candidate.family;
    software.id = stableSoftwareId(candidate.family, candidate.executablePath);
    software.displayName = candidate.displayName;
    software.version = executableVersion(candidate.executablePath);
    if (software.version.isEmpty()) software.version = candidate.version;
    if (software.version.isEmpty()) software.version = QStringLiteral("Not reported");
    software.architecture = executableArchitecture(candidate.executablePath);
    software.executablePath = QFileInfo(candidate.executablePath).absoluteFilePath();
    software.source = candidate.source;
    software.available = true;

        bool majorParsed = false;
        const int major = software.version.section(QLatin1Char('.'), 0, 0).toInt(&majorParsed);
        software.integrationAdapters = canoeIntegrationAdapters(software.version, software.architecture);
        software.compatibility = software.integrationAdapters.contains(QStringLiteral("vxl-process-route"))
            ? (majorParsed && major == 12
                ? QStringLiteral("CANoe 12 app-local route has bench evidence; named Provider remains a candidate")
                : QStringLiteral("CANoe 11 app-local route; qualification is limited to the tested installation"))
            : QStringLiteral("No qualified compatibility route for this CANoe version/architecture");
        software.compatibilityKind = QStringLiteral("warning");

    return software;
}

}  // namespace

QStringList canoeIntegrationAdapters(const QString& version, const QString& architecture) {
    QStringList adapters;
    if (architecture.compare(QStringLiteral("x64"), Qt::CaseInsensitive) == 0) {
        const auto major = version.section(QLatin1Char('.'), 0, 0);
        if (major == QStringLiteral("11") || major == QStringLiteral("12"))
            adapters << QStringLiteral("vxl-process-route");
        if (major == QStringLiteral("12")) adapters << QStringLiteral("canoe-provider");
    }
    return adapters << QStringLiteral("native");
}


SoftwareInstallation inspectProfileExecutable(const QString& executablePath,
                                                const QString& source, const QString& family) {
    const QFileInfo info(executablePath);
    if (!info.isFile()) return {};

    const QString fileName = info.fileName().toLower();
    if (family == QStringLiteral("canoe") && (fileName == QStringLiteral("canoe64.exe") ||
        fileName == QStringLiteral("canoe32.exe"))) {
        return projectCandidate({QStringLiteral("canoe"), QStringLiteral("Vector CANoe"),
                                 QString(), info.absoluteFilePath(), source});
    }


    if (family != QStringLiteral("custom")) return {};
    SoftwareInstallation software;
    software.id = stableSoftwareId(QStringLiteral("custom"), info.absoluteFilePath());
    software.family = QStringLiteral("custom");
    software.displayName = info.completeBaseName();
    software.version = executableVersion(info.absoluteFilePath());
    if (software.version.isEmpty()) software.version = QStringLiteral("Not reported");
    software.architecture = executableArchitecture(info.absoluteFilePath());
    software.executablePath = info.absoluteFilePath();
    software.source = source;
    software.compatibility =
        QStringLiteral("Unsupported: no existing UCAL launch route recognizes this executable");
    software.compatibilityKind = QStringLiteral("danger");
    software.available = true;
    return software;
}

QVector<SoftwareInstallation> discoverProfileInstallations(const QString& family) {
    QVector<Candidate> candidates;
    const QStringList uninstallRoots = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
    };
    for (const QString& root : uninstallRoots) scanUninstallRoot(root, &candidates);

    const QStringList appPathRoots = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths"),
        QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"),
    };
    for (const QString& root : appPathRoots) scanAppPaths(root, &candidates);

    QMap<QString, SoftwareInstallation> unique;
    for (const Candidate& candidate : candidates) {
        if (candidate.family != family) continue;
        const SoftwareInstallation projected = projectCandidate(candidate);
        auto found = unique.find(projected.id);
        if (found == unique.end() || found->source == QStringLiteral("Windows App Paths")) {
            unique.insert(projected.id, projected);
        }
    }
    return unique.values().toVector();
}

}
