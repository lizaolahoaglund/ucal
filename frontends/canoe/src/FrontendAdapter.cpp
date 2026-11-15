#include "CanoeText.h"
#include "Profile.h"
#include "LaunchCopy.h"
#include "FrontendPlugins.h"
#include "SoftwareDiscovery.h"
#include "ProductLauncher.h"
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include <QApplication>
#include <QProgressDialog>
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
    FrontendResult deploy(const QJsonObject& context) override { return deployPublicContract(context, false); }
    FrontendResult restoreDeployment(const QJsonObject& context) override { return deployPublicContract(context, true); }
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

            if (adapter != QStringLiteral("canoe-provider") && adapter != QStringLiteral("vxl-process-route"))
                errors << canoeProfileText(language, "launch.blocker.routeUnsupported");
            if (c.value("softwareArchitecture").toString() != QStringLiteral("x64"))
                errors << canoeProfileText(language, "launch.blocker.canoeX64");
            if (adapter == QStringLiteral("canoe-provider") && major != QStringLiteral("12"))
                errors << canoeProfileText(language, "launch.blocker.canoe12");
            else if (adapter == QStringLiteral("vxl-process-route") &&
                     !canoeIntegrationAdapters(c.value("softwareVersion").toString(),
                        c.value("softwareArchitecture").toString()).contains(adapter))
                errors << canoeProfileText(language, "launch.blocker.canoeAppLocalVersion");

        if (errors.isEmpty() && c.value("inspectLaunchAssets").toBool(true) &&
            adapter == QStringLiteral("vxl-process-route")) {
            const auto registrationError = canoeDriverRegistrationError(language);
            if (!registrationError.isEmpty()) errors << registrationError;
        }
        if (errors.isEmpty() && c.value("inspectLaunchAssets").toBool(true) && c.value("plan").isObject()) {
            // Profile preparation only constructs a description and reads assets;
            // it never deploys files, starts processes or reserves resources.
            const auto plan = launchPlanFromFrontendJson(c.value("plan").toObject());
            if (!plan.endpoints.isEmpty()) {
                const auto spec = prepareProfileLaunch(package_.implementation, plan,
                    c.value("applicationDirectory").toString(), QDir::tempPath(), language, {});
                if (!spec.error.isEmpty()) {
                    QString prerequisite;
                    if ((!plan.configurationPath.isEmpty() && !QFileInfo(plan.configurationPath).isFile()) ||
                        !inspectCanoeLaunchCopyInputs(plan, c.value("applicationDirectory").toString(),
                            installedCanoeRuntimes(), &prerequisite))
                        errors << (prerequisite.isEmpty() ? spec.error : canoeProfileText(language, "launch.product.canoePrepareFailed").arg(prerequisite));
                }
            }
        }
        return {{{"blockers", QJsonArray::fromStringList(errors)},
                 {"frontend", QStringLiteral("CANoe")}},
                {},
                0};
    }
    FrontendResult prepareLaunch(const QJsonObject &c) override {
        const auto p = launchPlanFromFrontendJson(c.value("plan").toObject());
        if (p.integrationAdapterId == QStringLiteral("vxl-process-route")) {
            // Recheck even when a complete launch copy is already cached.
            const auto error = canoeDriverRegistrationError(
                static_cast<UiLanguage>(c.value("language").toInt()));
            if (!error.isEmpty()) return {{}, error, 3};
        }
        QSet<int> reserved;
        for (const auto &v : c.value("reservedResourceSlots").toArray())
            reserved.insert(v.toInt());
        auto spec = prepareProfileLaunch(
            package_.implementation, p, c.value("applicationDirectory").toString(),
            c.value("temporaryDirectory").toString(), static_cast<UiLanguage>(c.value("language").toInt()),
            reserved);
        if (!spec.valid() && (p.configurationPath.isEmpty() || QFileInfo(p.configurationPath).isFile())) {
            const auto language = static_cast<UiLanguage>(c.value("language").toInt());
            QProgressDialog progress(canoeProfileText(language, "launch.product.canoePreparing"),
                QApplication::translate("QPlatformTheme", "Cancel"), 0, 0, QApplication::activeWindow());
            progress.setWindowModality(Qt::ApplicationModal);
            progress.setMinimumDuration(1000);
            QString error;
            const bool prepared = provisionCanoeLaunchCopy(p, c.value("applicationDirectory").toString(),
                installedCanoeRuntimes(), &error, [&progress] {
                    progress.setValue(0);
                    QApplication::processEvents();
                    return !progress.wasCanceled();
                });
            if (!prepared) spec.error = canoeProfileText(language, "launch.product.canoePrepareFailed").arg(error);
            else spec = prepareProfileLaunch(package_.implementation, p, c.value("applicationDirectory").toString(),
                c.value("temporaryDirectory").toString(), language, reserved);
        }
        if (spec.valid()) synchronizeCanoeLaunchAssets(p, c.value("applicationDirectory").toString(),
            static_cast<UiLanguage>(c.value("language").toInt()), &spec);
        return {productLaunchSpecToJson(spec), spec.error, spec.error.isEmpty() ? 0 : 3};
    }
};

}
std::shared_ptr<Frontend> createProfileFrontend(const FrontendPackage& p) { return std::make_shared<ProfileFrontend>(p); }
}

namespace Ucal::Manager { const char* frontendImplementation() { return "canoe"; } }
