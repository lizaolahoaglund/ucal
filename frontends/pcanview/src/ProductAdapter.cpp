#include "PcanViewText.h"
#include "Profile.h"
#include "ProductLauncher.h"
#include "FrontendPlugins.h"
#include <QJsonArray>
#include <QRegularExpression>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace Ucal::Manager {
namespace {
QString firstFile(const QStringList& candidates) {
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).isFile()) return QFileInfo(candidate).absoluteFilePath();
    }
    return QString();
}
QString fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return QString();
    return QString::fromLatin1(hash.result().toHex().toUpper());
}
QString installedPeakApi(const QString& selectedExecutable) {
    const QString override = qEnvironmentVariable("UCAL_MANAGER_PEAK_API").trimmed();
    if (!override.isEmpty() && QFileInfo(override).isFile()) {
        return QFileInfo(override).absoluteFilePath();
    }
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    return firstFile({
        QDir(QFileInfo(selectedExecutable).absolutePath()).filePath(QStringLiteral("PcanApi.dll")),
        QDir(programFiles).filePath(QStringLiteral("PEAK-System/PEAK-Drivers 5/Tools/PcanApi.dll")),
        QDir(programFiles).filePath(QStringLiteral("PEAK-System/PCAN-View 6/PcanApi.dll"))});
}
ProductLaunchSpec preparePcanView(const LaunchPlan& plan, const QString& applicationDirectory,
                                  const QString& temporaryDirectory,
                                  const UiLanguage language,
                                  const QSet<int>& reservedResourceSlots) {
    ProductLaunchSpec spec;
    const QString repositoryRoot = plan.repositoryRoot;
    const QString bridge = firstFile({
        QDir(applicationDirectory).filePath(QStringLiteral("frontends/pcanview/x64/UcalPcanViewNativeBridge.exe")),
        QDir(repositoryRoot).filePath(QStringLiteral("frontends/pcanview/x64/UcalPcanViewNativeBridge.exe")),
        QDir(applicationDirectory).filePath(
            QStringLiteral("runtime/x64/adapters/UcalPcanViewNativeBridge.exe")),
        QDir(repositoryRoot).filePath(
            QStringLiteral("runtime/x64/adapters/UcalPcanViewNativeBridge.exe")),
        QDir(repositoryRoot).filePath(
            QStringLiteral("out/x64/Release/UcalPcanViewNativeBridge.exe"))});
    const QString adapter = firstFile({
        QDir(applicationDirectory).filePath(QStringLiteral("frontends/pcanview/x64/UcalPcanViewNativeAdapter.dll")),
        QDir(repositoryRoot).filePath(QStringLiteral("frontends/pcanview/x64/UcalPcanViewNativeAdapter.dll")),
        QDir(applicationDirectory).filePath(
            QStringLiteral("runtime/x64/adapters/UcalPcanViewNativeAdapter.dll")),
        QDir(repositoryRoot).filePath(
            QStringLiteral("runtime/x64/adapters/UcalPcanViewNativeAdapter.dll")),
        QDir(repositoryRoot).filePath(
            QStringLiteral("out/x64/Release/UcalPcanViewNativeAdapter.dll"))});
    const QString support = QDir(QFileInfo(bridge).absolutePath()).filePath(
        QStringLiteral("UcalPcanViewSupport.dll"));
    const QString peakApi = installedPeakApi(plan.softwareExecutable);
    const QString digest = fileSha256(plan.softwareExecutable);
    if (bridge.isEmpty() || adapter.isEmpty() || !QFileInfo(support).isFile()) {
        spec.error = pcanViewProfileText(language, QStringLiteral("launch.product.pcanBridgeUnavailable"));
        return spec;
    }
    if (peakApi.isEmpty()) {
        spec.error = pcanViewProfileText(language, QStringLiteral("launch.product.peakRuntimeMissing"));
        return spec;
    }
    if (digest.isEmpty() || temporaryDirectory.trimmed().isEmpty()) {
        spec.error = pcanViewProfileText(language, QStringLiteral("launch.product.pcanVerificationFailed"));
        return spec;
    }

    const QString auditPath = QDir(temporaryDirectory).filePath(
        QStringLiteral("pcanview-bridge.jsonl"));
    spec.evidenceFiles << auditPath;
    spec.program = bridge;
    const QByteArray applicationDigest = QCryptographicHash::hash(
        plan.applicationId.toUtf8(), QCryptographicHash::Sha256);
    const int preferredNetIndex = applicationDigest.isEmpty()
        ? 1 : (static_cast<unsigned char>(applicationDigest.at(0)) % 16) + 1;
    int netIndex = 0;
    for (int offset = 0; offset < 16; ++offset) {
        const int candidate = ((preferredNetIndex - 1 + offset) % 16) + 1;
        if (!reservedResourceSlots.contains(candidate)) {
            netIndex = candidate;
            break;
        }
    }
    if (netIndex == 0) {
        spec.error = pcanViewProfileText(language, QStringLiteral("launch.product.pcanSlotsExhausted"));
        return spec;
    }
    spec.resourceSlot = netIndex;
    spec.arguments = QStringList{
        plan.softwareExecutable, auditPath,
        QStringLiteral("--adapter"), adapter,
        QStringLiteral("--expected-sha256"), digest,
        QStringLiteral("--expected-version"), plan.softwareVersion,
        QStringLiteral("--endpoint-id"), plan.endpoints.first().endpointId,
        QStringLiteral("--control-stdin"), QStringLiteral("true"),
        QStringLiteral("--channel-label"),
        QStringLiteral("UCAL CAN 1"),
        QStringLiteral("--peak-api"), peakApi,
        QStringLiteral("--net-index"), QString::number(netIndex)};
    for (const QString& argument : applicationArguments(plan))
        spec.arguments << QStringLiteral("--application-argument") << argument;
    spec.workingDirectory = applicationDirectory;
    spec.environment = QProcessEnvironment::systemEnvironment();
    spec.environment.insert(QStringLiteral("UCAL_PROFILE_ID"), plan.applicationId);
    return spec;
}
}
QString preferredProfileAdapter(const SoftwareInstallation& software) { return software.integrationAdapters.value(0); }
ProductLaunchSpec prepareProfileLaunch(const QString& implementation, const LaunchPlan& plan,
    const QString& applicationDirectory, const QString& temporaryDirectory, UiLanguage language,
    const QSet<int>& reserved) {
    if (implementation == QStringLiteral("pcanview") && !plan.endpoints.isEmpty())
        return preparePcanView(plan, applicationDirectory, temporaryDirectory, language, reserved);
    ProductLaunchSpec result;
    result.error = pcanViewProfileText(language, QStringLiteral("launch.product.routeUnavailable"));
    return result;
}
}
