#include "PcanViewText.h"
#include "Profile.h"
#include "FrontendPlugins.h"
#include "SoftwareDiscovery.h"
#include "ProductLauncher.h"
#include <QJsonArray>
#include <QDir>
namespace Ucal::Manager {
namespace {
QJsonObject softwareJson(const SoftwareInstallation &s) {
    return {{"id", s.id},
            {"family", s.family},
            {"displayName", s.displayName},
            {"version", s.version},
            {"architecture", s.architecture},
            {"executablePath", s.executablePath},
            {"source", s.source},
            {"compatibility", s.compatibility},
            {"compatibilityKind", s.compatibilityKind},
            {"integrationAdapters", QJsonArray::fromStringList(s.integrationAdapters)}};
}

class ProfileFrontend final : public Frontend {
    FrontendPackage package_;

  public:
    FrontendResult deploy(const QJsonObject& context) override { return Frontend::deploy(context); }
    FrontendResult restoreDeployment(const QJsonObject& context) override { return Frontend::restoreDeployment(context); }
    explicit ProfileFrontend(FrontendPackage p) : package_(std::move(p)) {}
    FrontendResult scan() override {
        QJsonArray array;
        for (const auto &s : discoverProfileInstallations(package_.implementation))
            array.append(softwareJson(s));
        return {{{"installations", array}}, {}, 0};
    }
    FrontendResult inspect(const QJsonObject &c) override {
        const auto s = inspectProfileExecutable(c.value("executablePath").toString(),
                                              c.value("source").toString(), package_.implementation);
        auto object = s.id.isEmpty() ? QJsonObject{} : softwareJson(s);
        if (!s.id.isEmpty()) {
            auto selection = s;
            const auto saved = c.value("selection").toObject();
            if (!saved.value("id").toString().isEmpty()) selection.id = saved.value("id").toString();
            if (saved.value("integrationAdapters").isArray()) {
                selection.integrationAdapters.clear();
                for (const auto& v : saved.value("integrationAdapters").toArray()) selection.integrationAdapters << v.toString();
            }
            object.insert("preferredAdapter", preferredProfileAdapter(selection));
        }
        return {object, {}, 0};
    }
    FrontendResult checkCompatibility(const QJsonObject &c) override {
        QStringList errors;
        const auto language = static_cast<UiLanguage>(c.value("language").toInt());
        const QString adapter = c.value("integrationAdapterId").toString();
        const QString major = c.value("softwareVersion").toString().section('.', 0, 0);

            if (adapter != QStringLiteral("pcanview5-6-structural-adapter") &&
                adapter != QStringLiteral("pcanview6-structural-adapter"))
                errors << pcanViewProfileText(language, "launch.blocker.routeUnsupported");
            if (c.value("softwareArchitecture").toString() != QStringLiteral("x64"))
                errors << pcanViewProfileText(language, "launch.blocker.pcanX64");
            if (major != QStringLiteral("5") && major != QStringLiteral("6"))
                errors << pcanViewProfileText(language, "launch.blocker.pcanVersion");
            // Both assignment modes resolve before launch. PCAN-View's limit
            // applies to that resolved catalog, not to how the user selected it.
            if (c.value("channelCount").toInt() != 1)
                errors << pcanViewProfileText(language, "launch.blocker.pcanOneEndpoint");
            if (c.value("plan").isObject() && c.value("plan").toObject().value("endpoints").toArray().size() != 1)
                errors << pcanViewProfileText(language, "launch.blocker.pcanResolution");

        if (errors.isEmpty() && c.value("inspectLaunchAssets").toBool(true) && c.value("plan").isObject()) {
            const auto plan = launchPlanFromFrontendJson(c.value("plan").toObject());
            if (!plan.endpoints.isEmpty()) {
                const auto spec = prepareProfileLaunch(package_.implementation, plan,
                    c.value("applicationDirectory").toString(), QDir::tempPath(), language, {});
                if (!spec.error.isEmpty()) errors << spec.error;
            }
        }
        return {{{"blockers", QJsonArray::fromStringList(errors)},
                 {"frontend", QStringLiteral("PCANView")}},
                {},
                0};
    }
    FrontendResult prepareLaunch(const QJsonObject &c) override {
        const auto p = launchPlanFromFrontendJson(c.value("plan").toObject());
        QSet<int> reserved;
        for (const auto &v : c.value("reservedResourceSlots").toArray())
            reserved.insert(v.toInt());
        const auto spec = prepareProfileLaunch(
            package_.implementation, p, c.value("applicationDirectory").toString(),
            c.value("temporaryDirectory").toString(), static_cast<UiLanguage>(c.value("language").toInt()),
            reserved);
        return {productLaunchSpecToJson(spec), spec.error, spec.error.isEmpty() ? 0 : 3};
    }
};

}
std::shared_ptr<Frontend> createProfileFrontend(const FrontendPackage& p) { return std::make_shared<ProfileFrontend>(p); }
}

namespace Ucal::Manager { const char* frontendImplementation() { return "pcanview"; } }
