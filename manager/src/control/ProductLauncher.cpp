#include "ProductLauncher.h"
#include "FrontendPlugins.h"
#include <QJsonArray>

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

QStringList applicationArguments(const LaunchPlan& plan) {
    QStringList arguments;
    if (!plan.configurationPath.trimmed().isEmpty())
        arguments << QDir::toNativeSeparators(QFileInfo(plan.configurationPath).absoluteFilePath());
#ifdef Q_OS_WIN
    // Use Windows' native quoting rules. The dummy executable prevents a leading
    // argument from being parsed using the special argv[0] rules.
    const QString command = QStringLiteral("ucal.exe ") + plan.commandLineArguments;
    int count = 0;
    LPWSTR* parsed = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(command.utf16()), &count);
    if (parsed != nullptr) {
        for (int index = 1; index < count; ++index) arguments << QString::fromWCharArray(parsed[index]);
        LocalFree(parsed);
    }
#endif
    return arguments;
}

QString preferredInstalledAdapter(const SoftwareInstallation& software) {
    const auto result = invokeFrontend(software.family, QStringLiteral("inspect"),
        {{"executablePath", software.executablePath}, {"source", software.source},
         {"selection", QJsonObject{{"id", software.id},
             {"integrationAdapters", QJsonArray::fromStringList(software.integrationAdapters)}}}});
    if (result.ok() && !result.value.value("preferredAdapter").toString().isEmpty())
        return result.value.value("preferredAdapter").toString();
    return software.integrationAdapters.value(0);
}

LaunchPlan launchPlanFromFrontendJson(const QJsonObject& o) {
    LaunchPlan p;
    p.frontend = o.value("frontend").toString(); p.ready = o.value("ready").toBool();
    p.applicationId = o.value("applicationId").toString();
    p.softwareInstallationId = o.value("softwareInstallationId").toString();
    const auto software = o.value("software").toObject();
    p.softwareFamily = software.value("family").toString();
    p.softwareVersion = software.value("version").toString();
    p.softwareArchitecture = software.value("architecture").toString();
    p.softwareExecutable = software.value("executablePath").toString();
    p.configurationPath = o.value("configurationPath").toString();
    p.commandLineArguments = o.value("commandLineArguments").toString();
    p.integrationAdapterId = o.value("integrationAdapterId").toString();
    p.assignmentMode = assignmentModeFromKey(o.value("assignmentMode").toString());
    p.repositoryRoot = o.value("launcher").toObject().value("repositoryRoot").toString();
    for (const auto& f : o.value("backendFamilies").toArray()) p.backendFamilies << f.toString();
    for (const auto& v : o.value("endpoints").toArray()) {
        const auto e = v.toObject(); LaunchEndpoint endpoint;
        endpoint.logicalChannel = e.value("logicalChannel").toInt();
        endpoint.endpointId = e.value("endpointId").toString();
        endpoint.adapterId = e.value("adapterId").toString();
        endpoint.backendFamily = e.value("backendFamily").toString();
        endpoint.busType = e.value("busType").toString();
        endpoint.physicalIndex = e.value("physicalIndex").toString().toUInt();
        p.endpoints << endpoint;
    }
    return p;
}

QJsonObject productLaunchSpecToJson(const ProductLaunchSpec& spec) {
    QJsonObject environment;
    for (const auto& key : spec.environment.keys())
        if (!key.isEmpty() && !key.contains('=')) environment.insert(key, spec.environment.value(key));
    return {{"program", spec.program}, {"arguments", QJsonArray::fromStringList(spec.arguments)},
        {"workingDirectory", spec.workingDirectory}, {"environment", environment},
        {"evidenceFiles", QJsonArray::fromStringList(spec.evidenceFiles)},
        {"resourceSlot", spec.resourceSlot}, {"error", spec.error}};
}

ProductLaunchSpec productLaunchSpecFromJson(const QJsonObject& o) {
    ProductLaunchSpec spec;
    spec.program = o.value("program").toString();
    spec.workingDirectory = o.value("workingDirectory").toString();
    spec.error = o.value("error").toString();
    const auto slot = o.contains("resourceSlot") ? o.value("resourceSlot") : o.value("pcanNetIndex");
    spec.resourceSlot = slot.toInt();
    const bool validSlot = slot.isUndefined() ||
        (slot.isDouble() && slot.toDouble() == spec.resourceSlot && spec.resourceSlot >= 0 && spec.resourceSlot <= 16);
    const bool conflictingSlots = o.contains("resourceSlot") && o.contains("pcanNetIndex") &&
        o.value("resourceSlot") != o.value("pcanNetIndex");
    bool valid = o.value("arguments").isArray() && QFileInfo(spec.program).isFile() &&
        QFileInfo(spec.program).isAbsolute() && QFileInfo(spec.workingDirectory).isDir() &&
        QFileInfo(spec.workingDirectory).isAbsolute() && validSlot && !conflictingSlots;
    if (o.contains("evidenceFiles") && (!o.value("evidenceFiles").isArray() || o.value("evidenceFiles").toArray().size() > 16)) valid = false;
    for (const auto& file : o.value("evidenceFiles").toArray()) {
        const auto path = file.toString();
        if (!file.isString() || !QFileInfo(path).isAbsolute() || path.contains(QChar(0))) valid = false;
        spec.evidenceFiles << path;
    }
    if (o.contains("environment") && !o.value("environment").isObject()) valid = false;
    for (const auto& a : o.value("arguments").toArray()) {
        if (!a.isString() || a.toString().contains(QChar(0))) valid = false;
        spec.arguments << a.toString();
    }
    spec.environment = QProcessEnvironment::systemEnvironment();
    const auto environment = o.value("environment").toObject();
    for (auto i = environment.begin(); i != environment.end(); ++i) {
        if (!i.value().isString() || i.key().isEmpty() || i.key().contains('=') || i.key().contains(QChar(0)) ||
            i.value().toString().contains(QChar(0))) valid = false;
        spec.environment.insert(i.key(), i.value().toString());
    }
    if (!valid && spec.error.isEmpty()) spec.error = QStringLiteral("Invalid frontend launch description: program=%1; directory=%2; network slot=%3")
        .arg(spec.program, spec.workingDirectory).arg(spec.resourceSlot);
    return spec;
}

ProductLaunchSpec ProductLauncher::prepare(const LaunchPlan& plan,
    const QString& applicationDirectory, const QString& temporaryDirectory,
    const UiLanguage language, const QSet<int>& reservedResourceSlots) {
    ProductLaunchSpec spec;
    if (!plan.ready) { spec.error = plan.blockers.join(QLatin1Char('\n')); return spec; }
    const QString family = !plan.softwareFamily.isEmpty() ? plan.softwareFamily :
        plan.frontend.toLower();
    if (family.isEmpty() || frontendPackage(family).id.isEmpty()) {
        spec.error = uiText(language, QStringLiteral("launch.product.routeUnavailable"));
        return spec;
    }
    QJsonArray reserved;
    for (const auto index : reservedResourceSlots) reserved.append(index);
    const auto result = invokeFrontend(family, QStringLiteral("prepare_launch"),
        {{"plan", plan.toJson()}, {"applicationDirectory", applicationDirectory},
         {"temporaryDirectory", temporaryDirectory}, {"language", int(language)},
         {"reservedResourceSlots", reserved}});
    if (!result.ok()) { spec.error = result.error; return spec; }
    return productLaunchSpecFromJson(result.value);
}

}  // namespace Ucal::Manager
