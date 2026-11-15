#include "DeploymentAssets.h"
#include "FrontendPlugins.h"
#include "CanoeText.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>

namespace Ucal::Manager {
namespace {
void saveEvidence(const QString& directory, const QString& name, const QByteArray& data) {
    QFile file(QDir(directory).filePath(name));
    if (file.open(QIODevice::WriteOnly)) file.write(data);
}
}

FrontendResult deployPublicContract(const QJsonObject& context, bool restore) {
    const auto language = static_cast<UiLanguage>(context.value("language").toInt());
    const auto text = [language](const char* key) { return canoeProfileText(language, QLatin1String(key)); };
    const auto fail = [](const QString& error) { return FrontendResult{{}, error, 3}; };
    const auto root = context.value("applicationDirectory").toString();
    const auto workspace = context.value("workspacePath").toString();
    const auto id = context.value("applicationId").toString();
    const auto target = context.value("executablePath").toString();
    const auto script = resolveVxlDeploymentScript(root);
    if (!QFileInfo(target).isFile()) return fail(text("vxl.executableUnavailable"));
    if (!QFileInfo(script).isFile()) return fail(text("vxl.helperMissing"));
    QString action = restore ? QStringLiteral("Restore") :
        context.value("confirmed").toBool() ? QStringLiteral("Apply") : QStringLiteral("Inspect");
    QString backup = restore ? latestVxlDeploymentBackup(workspace, id) : QString();
    if (restore && backup.isEmpty()) return fail(text("vxl.noBackup"));
    if (restore && !context.value("confirmed").toBool()) {
        return {{{"confirmation", QJsonObject{{"message", text("vxl.restoreQuestion")},
                   {"operation", "restore_deployment"}, {"context", QJsonObject{{"confirmed", true}}}}}}, {}, 0};
    }
    VxlProductAssets assets;
    if (action == QStringLiteral("Apply")) {
        assets = resolveVxlProductAssets(root, context.value("machine").toString());
        if (!assets.ready()) return fail(assets.error);
        backup = nextVxlDeploymentBackup(workspace, id);
    }
    const auto evidence = context.value("evidenceDirectory").toString();
    if (evidence.isEmpty() || !QDir().mkpath(evidence)) return fail(text("vxl.evidenceDirFailed"));
    const auto shell = QDir(qEnvironmentVariable("SystemRoot")).filePath(
        QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    QStringList arguments{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), script,
        QStringLiteral("-Action"), action, QStringLiteral("-TargetExecutable"), target};
    if (action == QStringLiteral("Apply")) arguments << QStringLiteral("-FrontendDll") << assets.frontendPath
        << QStringLiteral("-DiscoveryDll") << assets.discoveryPath << QStringLiteral("-BackupDirectory") << backup;
    else if (restore) arguments << QStringLiteral("-BackupDirectory") << backup;
    QProcess process;
    process.setProgram(shell); process.setArguments(arguments); process.setWorkingDirectory(root);
    process.start();
    if (!process.waitForStarted(5000)) return fail(process.errorString());
    bool parsedTimeout = false;
    const int configuredTimeout = qEnvironmentVariableIntValue("UCAL_MANAGER_DEPLOYMENT_TIMEOUT_MS", &parsedTimeout);
    const int timeout = parsedTimeout && configuredTimeout >= 100 && configuredTimeout <= 300000 ? configuredTimeout : 60000;
    const bool finished = process.waitForFinished(timeout);
    if (!finished) { process.kill(); process.waitForFinished(5000); }
    const auto output = process.readAllStandardOutput();
    const auto error = process.readAllStandardError();
    saveEvidence(evidence, QStringLiteral("stdout.log"), output);
    saveEvidence(evidence, QStringLiteral("stderr.log"), error);
    saveEvidence(evidence, QStringLiteral("result.json"), QJsonDocument(QJsonObject{
        {"action", action}, {"targetExecutable", target}, {"backupDirectory", backup},
        {"exitCode", process.exitCode()}, {"normalExit", process.exitStatus() == QProcess::NormalExit},
        {"timedOut", !finished}}).toJson());
    if (!finished || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return fail(text("vxl.transactionFailed").arg(action,
            error.isEmpty() ? QString() : QStringLiteral("\n\n") + QString::fromLocal8Bit(error).left(4000), evidence));
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(output, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return fail(text("vxl.invalidResult").arg(evidence));
    const auto result = document.object();
    if (result.value("executableMutation").toBool(true)) return fail(text("vxl.executableMutation"));
    if (action == QStringLiteral("Inspect")) {
        const auto machine = result.value("machine").toString();
        if (result.value("schema").toString() != QStringLiteral("org.ucal.vxl-client-capability/1") ||
            result.value("status").toString() != QStringLiteral("compatible-static-import") ||
            (machine != QStringLiteral("x86") && machine != QStringLiteral("x64")) ||
            QFileInfo(result.value("targetExecutable").toString()).canonicalFilePath().compare(
                QFileInfo(target).canonicalFilePath(), Qt::CaseInsensitive) != 0)
            return fail(text("vxl.mismatchedContract"));
        const auto inspected = resolveVxlProductAssets(root, machine);
        if (!inspected.ready()) return fail(inspected.error);
        return {{{"confirmation", QJsonObject{{"message", text("vxl.applyQuestion").arg(machine)},
            {"operation", "deploy"}, {"context", QJsonObject{{"confirmed", true}, {"machine", machine}}}}}}, {}, 0};
    }
    if (result.value("status").toString() != (restore ? QStringLiteral("restored") : QStringLiteral("applied")))
        return fail(text("vxl.unexpectedStatus"));
    return {{{"message", text("vxl.completed").arg(text(restore ? "vxl.restored" : "vxl.applied"), backup)}}, {}, 0};
}
} // namespace Ucal::Manager
