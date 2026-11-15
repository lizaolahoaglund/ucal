#include "PcanViewText.h"
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



        if (name.startsWith(QStringLiteral("PEAK-Drivers"), Qt::CaseInsensitive)) {
            Candidate candidate{QStringLiteral("pcanview"), QStringLiteral("PEAK PCAN-View"),
                                QString(), QDir(location).filePath(QStringLiteral("Tools/PcanView.exe")),
                                QStringLiteral("Windows Registry")};
            appendIfExecutable(candidates, candidate);
        }
    }
}

void scanAppPaths(const QString& root, QVector<Candidate>* candidates) {
    QSettings settings(root, QSettings::NativeFormat);
    for (const QString& group : settings.childGroups()) {
        const QString lower = group.toLower();
        if (lower != QStringLiteral("pcanview.exe")) continue;
        settings.beginGroup(group);
        QString executable = settings.value(QStringLiteral("Default")).toString();
        if (executable.isEmpty()) executable = settings.value(QStringLiteral(".")).toString();
        settings.endGroup();
        appendIfExecutable(candidates,
            {QStringLiteral("pcanview"),
             QStringLiteral("PEAK PCAN-View"),
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

        const bool x64 = software.architecture.compare(
            QStringLiteral("x64"), Qt::CaseInsensitive) == 0;
        software.integrationAdapters = compatiblePcanViewIntegrationAdapters(
            software.version, software.architecture);
        if (software.integrationAdapters.contains(
                QStringLiteral("pcanview5-6-structural-adapter"))) {
            software.compatibility = QStringLiteral(
                "PCAN-View 5/6 x64 comparator; exact local hash/version pin required");
            software.compatibilityKind = QStringLiteral("warning");
        } else {
            software.compatibility = x64
                ? QStringLiteral("Unsupported: the structural profile is limited to PCAN-View 5/6 x64")
                : QStringLiteral("Unsupported: only the PCAN-View 5/6 x64 engineering comparator is available");
            software.compatibilityKind = QStringLiteral("danger");
        }

    return software;
}

}  // namespace

QStringList compatiblePcanViewIntegrationAdapters(const QString& version,
                                                   const QString& architecture) {
    bool majorParsed = false;
    const int major = version.section(QLatin1Char('.'), 0, 0).toInt(&majorParsed);
    if (architecture.compare(QStringLiteral("x64"), Qt::CaseInsensitive) == 0 &&
        majorParsed && (major == 5 || major == 6)) {
        return {QStringLiteral("pcanview5-6-structural-adapter"),
                QStringLiteral("native")};
    }
    return {QStringLiteral("native")};
}

SoftwareInstallation inspectProfileExecutable(const QString& executablePath,
                                                const QString& source, const QString& family) {
    const QFileInfo info(executablePath);
    if (!info.isFile()) return {};

    const QString fileName = info.fileName().toLower();

    if (family == QStringLiteral("pcanview") && fileName == QStringLiteral("pcanview.exe")) {
        return projectCandidate({QStringLiteral("pcanview"),
                                 QStringLiteral("PEAK PCAN-View"), QString(),
                                 info.absoluteFilePath(), source});
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

    for (const auto& variable : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
        const auto programFiles = qEnvironmentVariable(variable);
        if (programFiles.isEmpty()) continue;
        if (family == QStringLiteral("pcanview")) {
            for (const auto& folder : {"PEAK-System/PCAN-View 6", "PEAK-System/PCAN-View 5",
                                       "PEAK-System/PEAK-Drivers 5/Tools", "PEAK-System/PCAN-View"})
                appendIfExecutable(&candidates, {family, QStringLiteral("PEAK PCAN-View"), {},
                    QDir(programFiles).filePath(QString::fromLatin1(folder) + QStringLiteral("/PcanView.exe")),
                    QStringLiteral("Standard installation")});
        }
    }
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
