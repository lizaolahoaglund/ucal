#include "Profile.h"
#include "../control/FrontendPlugins.h"
#include "ucal/FrontendPlugin.h"
#include "../control/PluginMetadata.h"
#include <QJsonDocument>
#include <cstring>
namespace Ucal::Manager { std::shared_ptr<Frontend> createProfileFrontend(const FrontendPackage&); }
namespace {
int32_t invokeVersioned(uint32_t operation, const char* input, uint32_t bytes,
                        char* output, uint32_t capacity, uint32_t* written, bool canonical) {
    if (written) *written = 0;
    if (!written || !input || bytes > UCAL_FRONTEND_MAX_JSON || !output || capacity < UCAL_FRONTEND_MAX_JSON) return 2;
    try {
        QJsonParseError parse;
        const auto request = QJsonDocument::fromJson(QByteArray(input, int(bytes)), &parse);
        if (parse.error != QJsonParseError::NoError || !request.isObject()) return 2;
        Ucal::Manager::FrontendPackage package;
        package.implementation = QString::fromLatin1(Ucal::Manager::frontendImplementation());
        Ucal::Manager::FrontendResult result;
        auto context = request.object();
        if (canonical) {
            QJsonValue converted;
            if (!Ucal::Manager::convertPluginJson(context, &converted, false)) return 2;
            context = converted.toObject();
        }
        package.manifestPath = context.value("manifestPath").toString();
        const auto frontend = Ucal::Manager::createProfileFrontend(package);
        switch (operation) {
        case UCAL_FRONTEND_SCAN: result = frontend->scan(); break;
        case UCAL_FRONTEND_INSPECT: result = frontend->inspect(context); break;
        case UCAL_FRONTEND_CHECK_COMPATIBILITY: result = frontend->checkCompatibility(context); break;
        case UCAL_FRONTEND_PREPARE_LAUNCH: result = frontend->prepareLaunch(context); break;
        case UCAL_FRONTEND_DEPLOY: result = frontend->deploy(context); break;
        case UCAL_FRONTEND_RESTORE_DEPLOYMENT: result = frontend->restoreDeployment(context); break;
        default: return 1;
        }
        auto value = result.value;
        if (!result.error.isEmpty()) value.insert("error", result.error);
        if (canonical) {
            QJsonValue converted;
            if (!Ucal::Manager::convertPluginJson(value, &converted, true)) return 3;
            value = converted.toObject();
        }
        const auto json = QJsonDocument(value).toJson(QJsonDocument::Compact);
        if (json.size() > int(capacity)) return 3;
        std::memcpy(output, json.constData(), size_t(json.size()));
        *written = uint32_t(json.size());
        return result.code;
    } catch (...) { return 3; }
}
int32_t UCAL_FRONTEND_CALL invokeV1(uint32_t operation, const char* input, uint32_t bytes,
                                    char* output, uint32_t capacity, uint32_t* written) {
    return invokeVersioned(operation, input, bytes, output, capacity, written, false);
}
int32_t UCAL_FRONTEND_CALL invokeV2(uint32_t operation, const char* input, uint32_t bytes,
                                    char* output, uint32_t capacity, uint32_t* written) {
    return invokeVersioned(operation, input, bytes, output, capacity, written, true);
}
}
extern "C" int32_t UCAL_FRONTEND_CALL UcalFrontendGetApi(uint32_t abi, UcalFrontendApiV1* api) {
    if (!api || api->size != sizeof(*api)) return 2;
    if (abi != UCAL_FRONTEND_ABI_V1 && abi != UCAL_FRONTEND_ABI_V2) return 1;
    *api = {sizeof(*api), abi, 0, abi == UCAL_FRONTEND_ABI_V2 ? invokeV2 : invokeV1}; return 0;
}
