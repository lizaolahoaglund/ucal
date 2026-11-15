#include "FrontendPlugins.h"
#include "LaunchPlan.h"
#include "ProductLauncher.h"
#include "ProductOperations.h"
#include "SoftwareDiscovery.h"
#include "ucal/FrontendPlugin.h"
#include "PluginMetadata.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>
#include <QRegularExpression>
#include <QSysInfo>

namespace Ucal::Manager {
namespace {
const QStringList operationNames{
    QStringLiteral("scan"),          QStringLiteral("inspect"), QStringLiteral("check_compatibility"),
    QStringLiteral("prepare_launch"), QStringLiteral("deploy"),  QStringLiteral("restore_deployment")};
FrontendResult unsupported() { return {{}, QStringLiteral("Operation is not provided by this frontend"), 1}; }
class DllFrontend final : public Frontend {
    FrontendPackage package_;
    QLibrary library_;

  protected:
    FrontendResult invoke(unsigned operation, const QJsonObject &context) override {
        if (!library_.isLoaded() && !library_.load())
            return {{}, library_.errorString(), 3};
        auto get = reinterpret_cast<UcalFrontendGetApiFunctionV1>(library_.resolve("UcalFrontendGetApi"));
        if (!get)
            get = reinterpret_cast<UcalFrontendGetApiFunctionV1>(library_.resolve("ucal_frontend_get_api"));
        UcalFrontendApiV1 api{};
        api.size = sizeof(api);
        if (!get) return {{}, QStringLiteral("Incompatible frontend DLL interface"), 3};
        uint32_t requestedAbi = UCAL_FRONTEND_ABI_V2;
        if (get(requestedAbi, &api) != 0) {
            requestedAbi = UCAL_FRONTEND_ABI_V1;
            api = {}; api.size = sizeof(api);
            if (get(requestedAbi, &api) != 0)
                return {{}, QStringLiteral("Incompatible frontend DLL interface"), 3};
        }
        if (api.size != sizeof(api) || api.abiVersion != requestedAbi || api.reserved || !api.invoke)
            return {{}, QStringLiteral("Incompatible frontend DLL interface"), 3};
        auto request = context;
        request.insert("manifestPath", package_.manifestPath);
        if (requestedAbi == UCAL_FRONTEND_ABI_V2) {
            QJsonValue converted;
            QString error;
            if (!convertPluginJson(request, &converted, true, &error)) return {{}, error, 2};
            request = converted.toObject();
        }
        const auto input = QJsonDocument(request).toJson(QJsonDocument::Compact);
        if (input.size() > int(UCAL_FRONTEND_MAX_JSON))
            return {{}, QStringLiteral("Frontend request is too large"), 2};
        QByteArray output(int(UCAL_FRONTEND_MAX_JSON), '\0');
        uint32_t written = 0;
        const auto code = api.invoke(operation, input.constData(), uint32_t(input.size()), output.data(),
                                     uint32_t(output.size()), &written);
        if (written > uint32_t(output.size()))
            return {{}, QStringLiteral("Invalid frontend output length"), 3};
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(output.left(int(written)), &parse);
        if ((written && (parse.error != QJsonParseError::NoError || !document.isObject())) ||
            (!written && !code))
            return {{}, QStringLiteral("Invalid frontend JSON response"), 3};
        auto object = document.object();
        if (requestedAbi == UCAL_FRONTEND_ABI_V2) {
            QJsonValue converted;
            QString error;
            if (!convertPluginJson(object, &converted, false, &error)) return {{}, error, 3};
            object = converted.toObject();
        }
        QString error = object.value("error").toString();
        if (code) {
            if (error.isEmpty())
                error = QStringLiteral("Frontend operation failed");
            error += QStringLiteral(" (code %1)").arg(code);
            if (object.contains("vendorCode"))
                error += QStringLiteral("; vendor code: %1")
                             .arg(object.value("vendorCode").toVariant().toString());
        }
        return {object, error, int(code)};
    }

  public:
    explicit DllFrontend(FrontendPackage p) : package_(std::move(p)), library_(package_.libraryPath) {
        library_.setLoadHints(QLibrary::ResolveAllSymbolsHint);
    }
};
} // namespace

FrontendResult Frontend::invoke(unsigned, const QJsonObject &) { return unsupported(); }
FrontendResult Frontend::scan() { return invoke(UCAL_FRONTEND_SCAN, {}); }
FrontendResult Frontend::inspect(const QJsonObject &c) { return invoke(UCAL_FRONTEND_INSPECT, c); }
FrontendResult Frontend::checkCompatibility(const QJsonObject &c) { return invoke(UCAL_FRONTEND_CHECK_COMPATIBILITY, c); }
FrontendResult Frontend::prepareLaunch(const QJsonObject &c) {
    return invoke(UCAL_FRONTEND_PREPARE_LAUNCH, c);
}
FrontendResult Frontend::deploy(const QJsonObject &c) { return invoke(UCAL_FRONTEND_DEPLOY, c); }
FrontendResult Frontend::restoreDeployment(const QJsonObject &c) {
    return invoke(UCAL_FRONTEND_RESTORE_DEPLOYMENT, c);
}

QString frontendDirectory(const QString &applicationDirectory) {
    const auto configured = qEnvironmentVariable("UCAL_FRONTEND_DIRECTORY");
    if (!configured.isEmpty() && QFileInfo(configured).isAbsolute())
        return QDir::cleanPath(configured);
    QDir directory(applicationDirectory);
    for (int depth = 0; depth < 8; ++depth) {
        const auto path = directory.filePath(QStringLiteral("frontends"));
        if (QFileInfo(path).isDir())
            return path;
        if (!directory.cdUp())
            break;
    }
    return QDir(applicationDirectory).filePath(QStringLiteral("../frontends"));
}

FrontendDirectoryScan scanFrontendDirectory(const QString &directory) {
    FrontendDirectoryScan result;
    QSet<QString> ids;
    const QSet<QString> keys{"schema",         "id",
                             "name",           "version",
                             "implementation", "library",
                             "operations",     "maximumCanChannels",
                             "fullAccess",     "configurationTool", "launcherProfiles", "description", "launch",
                             "integrations", "compatibilityReady", "compatibilityUnavailable", "configurationFile", "refreshIntegration"};
    for (const auto &folder :
         QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        const auto path = QDir(folder.absoluteFilePath()).filePath(QStringLiteral("frontend.json"));
        QFile file(path);
        QJsonParseError parse;
        const auto document = file.open(QIODevice::ReadOnly) && file.size() <= 65536
                                  ? QJsonDocument::fromJson(file.readAll(), &parse)
                                  : QJsonDocument();
        QJsonValue normalized;
        QString normalizationError;
        const bool normalizedOk = convertPluginJson(document.object(), &normalized, false, &normalizationError);
        const auto o = normalized.toObject();
        FrontendPackage p;
        p.id = o.value("id").toString();
        p.name = o.value("name").toString();
        p.version = o.value("version").toString();
        p.manifestPath = path;
        p.implementation = o.value("implementation").toString();
        p.description = o.value("description");
        p.integrations = o.value("integrations").toObject();
        p.compatibilityReady = o.value("compatibilityReady");
        p.compatibilityUnavailable = o.value("compatibilityUnavailable");
        p.configurationFile = o.value("configurationFile").toObject();
        p.refreshIntegration = o.value("refreshIntegration").toBool();
        QString error;
        bool valid = document.isObject() && normalizedOk &&
                     (!o.contains("launch") || o.value("launch").isObject()) &&
                     (o.value("schema").toString() == QStringLiteral("org.ucal.frontend-package/1") ||
                      o.value("schema").toString() == QStringLiteral("org.ucal.frontend-package/2")) &&
                     QRegularExpression(QStringLiteral("^[A-Za-z0-9._-]{1,127}$")).match(p.id).hasMatch() &&
                     !ids.contains(p.id) && !p.name.trimmed().isEmpty() && p.name.size() <= 127 &&
                     !p.name.contains(QChar(0)) && !p.version.isEmpty() && p.version.size() <= 64 &&
                     !p.version.contains(QChar(0)) && o.value("operations").isArray() &&
                     o.value("maximumCanChannels").isDouble() && o.value("fullAccess").isBool();
        for (auto i = o.begin(); i != o.end(); ++i)
            if (!keys.contains(i.key()))
                valid = false;
        for (const auto& key : {"description", "compatibilityReady", "compatibilityUnavailable"})
            if (!validLocalizedMetadata(o.value(key))) valid = false;
        for (auto it = p.integrations.begin(); it != p.integrations.end(); ++it)
            if (!validLocalizedMetadata(it.value())) valid = false;
        if (o.contains("integrations") && !o.value("integrations").isObject()) valid = false;
        if (o.contains("refreshIntegration") && !o.value("refreshIntegration").isBool()) valid = false;
        if (o.contains("configurationFile")) {
            if (!o.value("configurationFile").isObject() || !p.configurationFile.value("enabled").isBool() ||
                !validLocalizedMetadata(p.configurationFile.value("filter"))) valid = false;
        }
        const auto maximum = o.value("maximumCanChannels").toDouble();
        if (maximum < 1 || maximum > 16 || maximum != int(maximum))
            valid = false;
        p.maximumCanChannels = maximum >= 1 && maximum <= 16 ? int(maximum) : 1;
        p.fullAccess = o.value("fullAccess").toBool();
        for (const auto &op : o.value("operations").toArray()) {
            const auto operation = canonicalOperation(op.toString());
            if (!op.isString() || !operationNames.contains(operation) ||
                p.operations.contains(operation))
                valid = false;
            p.operations.insert(operation);
        }
        if (p.implementation == QStringLiteral("dll")) {
            p.libraryPath = QDir(folder.absoluteFilePath()).absoluteFilePath(o.value("library").toString().replace(QStringLiteral("{architecture}"), sizeof(void*) == 8 ? QStringLiteral("x64") : QStringLiteral("x86")));
            const auto machine = sizeof(void *) == 8 ? QStringLiteral("x64") : QStringLiteral("x86");
            if (!o.value("library").isString() || o.value("library").toString().contains(QChar(0)) ||
                runtimeLibraryArchitecture(p.libraryPath) != machine)
                valid = false;
        } else valid = false;
        if (!parsePackageTool(o.value("configurationTool"), path, p.configurationTool, error))
            valid = false;
        if (!valid) {
            result.diagnostics << path +
                                      QStringLiteral(
                                          ": Invalid frontend package or matching DLL is missing") +
                                      error;
            continue;
        }
        ids.insert(p.id);
        result.packages << p;
    }
    return result;
}

std::shared_ptr<Frontend> createFrontend(const FrontendPackage &p) {
    return std::make_shared<DllFrontend>(p);
}
FrontendPackage frontendPackage(const QString &id) {
    const auto scan = scanFrontendDirectory(frontendDirectory(QCoreApplication::applicationDirPath()));
    for (const auto &p : scan.packages)
        if (p.id == id)
            return p;
    return {};
}
FrontendResult invokeFrontend(const QString &id, const QString &operation, const QJsonObject &context) {
    const auto package = frontendPackage(id);
    if (package.id.isEmpty())
        return {{}, QStringLiteral("Frontend package is unavailable: %1").arg(id), 3};
    if (!package.operations.contains(operation))
        return unsupported();
    auto frontend = createFrontend(package);
    if (operation == QStringLiteral("scan"))
        return frontend->scan();
    if (operation == QStringLiteral("inspect"))
        return frontend->inspect(context);
    if (operation == QStringLiteral("check_compatibility"))
        return frontend->checkCompatibility(context);
    if (operation == QStringLiteral("prepare_launch"))
        return frontend->prepareLaunch(context);
    if (operation == QStringLiteral("deploy"))
        return frontend->deploy(context);
    if (operation == QStringLiteral("restore_deployment"))
        return frontend->restoreDeployment(context);
    return unsupported();
}
} // namespace Ucal::Manager
