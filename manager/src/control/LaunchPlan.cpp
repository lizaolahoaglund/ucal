#include "PortablePaths.h"
#include "LaunchPlan.h"
#include "FrontendPlugins.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace Ucal::Manager {
namespace {

constexpr int kMaximumLaunchChannels = 16;

void appendBlocker(LaunchPlan* plan, const QString& message) {
    if (plan != nullptr && !plan->blockers.contains(message)) plan->blockers.push_back(message);
}

bool canonicalUuid(const QString& value) {
    const QUuid parsed(value);
    return !parsed.isNull() &&
           parsed.toString(QUuid::WithoutBraces).compare(value, Qt::CaseInsensitive) == 0;
}

void appendEndpoint(LaunchPlan* plan, const Endpoint* endpoint, const int logicalChannel,
                    QSet<QString>* endpointIds, const UiLanguage language) {
    if (plan == nullptr || endpoint == nullptr) return;
    if (!canonicalUuid(endpoint->id) || !canonicalUuid(endpoint->adapterId) ||
        endpoint->backendFamily.isEmpty()) {
        appendBlocker(plan, uiText(language, QStringLiteral("launch.blocker.identityIncomplete"))
                                .arg(logicalChannel));
        return;
    }
    if (endpoint->state != EndpointState::Online) {
        appendBlocker(plan, uiText(language, QStringLiteral("launch.blocker.endpointOffline"))
                                .arg(logicalChannel));
        return;
    }
    if (endpoint->busType.compare(QStringLiteral("CAN"), Qt::CaseInsensitive) != 0) {
        appendBlocker(plan, uiText(language, QStringLiteral("launch.blocker.notCan"))
                                .arg(logicalChannel));
        return;
    }
    if (!endpoint->physicalIndexValid) {
        appendBlocker(plan, uiText(language, QStringLiteral("launch.blocker.rawIndexMissing"))
                                .arg(logicalChannel));
        return;
    }
    if (endpointIds != nullptr && endpointIds->contains(endpoint->id)) {
        // The complete mapping has already produced one aggregate diagnostic.
        // Keep only one resolved endpoint instead of adding partial pair warnings.
        return;
    }
    if (endpointIds != nullptr) endpointIds->insert(endpoint->id);
    plan->endpoints.push_back({logicalChannel, endpoint->id, endpoint->adapterId,
                               endpoint->backendFamily, endpoint->serial,
                               endpoint->busType, endpoint->physicalIndex,
                               endpoint->physicalChannel});
    if (!plan->backendFamilies.contains(endpoint->backendFamily)) {
        plan->backendFamilies.push_back(endpoint->backendFamily);
    }
}

}  // namespace

QJsonObject LaunchEndpoint::toJson() const {
    QJsonObject object;
    object.insert(QStringLiteral("logicalChannel"), logicalChannel);
    object.insert(QStringLiteral("endpointId"), endpointId);
    object.insert(QStringLiteral("adapterId"), adapterId);
    object.insert(QStringLiteral("backendFamily"), backendFamily);
    object.insert(QStringLiteral("serial"), serial);
    object.insert(QStringLiteral("busType"), busType);
    object.insert(QStringLiteral("physicalIndex"), QString::number(physicalIndex));
    return object;
}

QJsonObject LaunchPlan::toJson() const {
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("frontend"), frontend);
    root.insert(QStringLiteral("sessionId"), sessionId);
    root.insert(QStringLiteral("createdUtc"), createdUtc);
    root.insert(QStringLiteral("ready"), ready);
    root.insert(QStringLiteral("nativeLaunch"), nativeLaunch);
    root.insert(QStringLiteral("brokerInventoryFresh"), brokerInventoryFresh);
    root.insert(QStringLiteral("applicationId"), applicationId);
    root.insert(QStringLiteral("applicationName"), applicationName);
    root.insert(QStringLiteral("softwareInstallationId"), softwareInstallationId);
    QJsonObject software;
    software.insert(QStringLiteral("family"), softwareFamily);
    software.insert(QStringLiteral("version"), softwareVersion);
    software.insert(QStringLiteral("architecture"), softwareArchitecture);
    software.insert(QStringLiteral("executablePath"), softwareExecutable);
    root.insert(QStringLiteral("software"), software);
    root.insert(QStringLiteral("configurationPath"), configurationPath);
    root.insert(QStringLiteral("commandLineArguments"), commandLineArguments);
    root.insert(QStringLiteral("integrationAdapterId"), integrationAdapterId);
    root.insert(QStringLiteral("assignmentMode"), assignmentModeKey(assignmentMode));
    root.insert(QStringLiteral("backendFamilies"), QJsonArray::fromStringList(backendFamilies));
    QJsonArray resolvedEndpoints;
    for (const LaunchEndpoint& endpoint : endpoints) resolvedEndpoints.append(endpoint.toJson());
    root.insert(QStringLiteral("endpoints"), resolvedEndpoints);
    root.insert(QStringLiteral("blockers"), QJsonArray::fromStringList(blockers));
    QJsonObject launcher;
    launcher.insert(QStringLiteral("repositoryRoot"), repositoryRoot);
    root.insert(QStringLiteral("launcher"), launcher);
    return root;
}

QString LaunchPlan::summary(const UiLanguage language) const {
    QStringList lines;
    lines << uiText(language, QStringLiteral("launch.summary.application")).arg(applicationName)
          << uiText(language, QStringLiteral("launch.summary.frontend")).arg(frontend)
          << uiText(language, QStringLiteral("launch.summary.software"))
                 .arg(softwareFamily, softwareVersion, softwareArchitecture)
          << uiText(language, QStringLiteral("launch.summary.adapter")).arg(integrationAdapterId)
          << uiText(language, QStringLiteral("launch.summary.assignment"))
                 .arg(uiText(language, assignmentMode == AssignmentMode::FullAccess
                                           ? QStringLiteral("editor.full")
                                           : QStringLiteral("editor.selected")))
          << uiText(language, QStringLiteral("launch.summary.channelCount")).arg(endpoints.size())
          << uiText(language, QStringLiteral("launch.summary.status"))
                 .arg(uiText(language, ready ? QStringLiteral("launch.summary.ready")
                                             : QStringLiteral("launch.summary.blocked")));
    for (const LaunchEndpoint& endpoint : endpoints) {
        lines << uiText(language, QStringLiteral("launch.summary.channel"))
                     .arg(endpoint.logicalChannel)
                     .arg(endpoint.backendFamily)
                     .arg(endpoint.physicalIndex);
    }
    if (!blockers.isEmpty()) {
        lines << uiText(language, QStringLiteral("launch.summary.blockers"));
        for (const QString& blocker : blockers) lines << QStringLiteral("  - %1").arg(blocker);
    }
    return lines.join(QLatin1Char('\n'));
}

LaunchPlan LaunchCoordinator::prepare(const ManagerModel& model, const QString& applicationId,
                                      const bool brokerInventoryFresh,
                                      const bool nativeLaunch,
                                      const LaunchEnvironment& environment) {
    LaunchPlan plan;
    const UiLanguage language = environment.language;
    plan.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    plan.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    plan.applicationId = applicationId;
    plan.nativeLaunch = nativeLaunch;
    plan.brokerInventoryFresh = brokerInventoryFresh;
    plan.frontend = nativeLaunch ? QStringLiteral("Native") : QString();

    const ApplicationProfile* application = model.applicationById(applicationId);
    if (application == nullptr) {
        appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.profileNotFound")));
        return plan;
    }
    plan.applicationName = application->name;
    plan.softwareInstallationId = application->softwareInstallationId;
    plan.integrationAdapterId = nativeLaunch ? QStringLiteral("native")
                                             : application->integrationAdapterId;
    plan.assignmentMode = application->assignmentMode;

    const SoftwareInstallation* software = model.softwareById(application->softwareInstallationId);
    if (software == nullptr) {
        appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.softwareNotFound")));
        return plan;
    }
    plan.softwareFamily = software->family;
    plan.softwareVersion = software->version;
    plan.softwareArchitecture = software->architecture;
    plan.softwareExecutable = QFileInfo(software->executablePath).absoluteFilePath();
    plan.configurationPath = application->configurationPath;
    plan.commandLineArguments = application->commandLineArguments;
    if (!software->available || !QFileInfo(plan.softwareExecutable).isFile()) {
        appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.executableUnavailable")));
    }

    if (nativeLaunch) {
        plan.ready = plan.blockers.isEmpty();
        return plan;
    }

    if (!brokerInventoryFresh) {
        appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.inventoryStale")));
    }
    const auto package = frontendPackage(software->family);
    const bool supportsFullAccess = package.fullAccess;
    if (package.id.isEmpty())
        appendBlocker(&plan, QStringLiteral("Frontend package is unavailable: %1").arg(software->family));
    plan.frontend = package.name;
    if (!package.id.isEmpty() && !package.operations.contains(QStringLiteral("prepare_launch")))
        appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.routeUnsupported")));
    if (application->assignmentMode == AssignmentMode::Selected && application->channelCount > package.maximumCanChannels)
        appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.selectedCount")));

    const auto mappings = application->assignmentMode == AssignmentMode::Selected
        ? application->mappings : model.effectiveMappingsForApplication(applicationId);
    const auto conflicts = duplicateEndpointChannels(mappings,
        application->assignmentMode == AssignmentMode::Selected ? application->channelCount : mappings.size());
    if (!conflicts.isEmpty()) appendBlocker(&plan, duplicateEndpointMessage(language, conflicts));
    QSet<QString> endpointIds;
    if (application->assignmentMode == AssignmentMode::Selected) {
        if (application->channelCount < 1 || application->channelCount > kMaximumLaunchChannels) {
            appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.selectedCount")));
        } else {
            for (int logicalChannel = 1; logicalChannel <= application->channelCount; ++logicalChannel) {
                const auto mapping = std::find_if(
                    application->mappings.cbegin(), application->mappings.cend(),
                    [logicalChannel](const ChannelMapping& item) {
                        return item.logicalChannel == logicalChannel;
                    });
                if (mapping == application->mappings.cend() || mapping->endpointId.isEmpty()) {
                    appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.channelUnmapped"))
                                            .arg(logicalChannel));
                    continue;
                }
                const Endpoint* endpoint = model.endpointById(mapping->endpointId);
                if (endpoint == nullptr) {
                    appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.channelUnknown"))
                                            .arg(logicalChannel));
                    continue;
                }
                appendEndpoint(&plan, endpoint, logicalChannel,
                               &endpointIds, language);
            }
        }
    } else if (supportsFullAccess) {
        const QVector<ChannelMapping> effective =
            model.effectiveMappingsForApplication(applicationId);
        if (effective.isEmpty()) {
            appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.noOnlineEndpoint")));
        } else if (effective.size() > package.maximumCanChannels) {
            appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.tooManyChannels")));
        } else {
            for (const ChannelMapping& mapping : effective) {
                const Endpoint* endpoint = model.endpointById(mapping.endpointId);
                appendEndpoint(&plan, endpoint, mapping.logicalChannel,
                               &endpointIds, language);
            }
        }
    } else {
        appendBlocker(&plan, uiText(language, QStringLiteral("launch.blocker.routeUnsupported")));
    }

    plan.backendFamilies.sort(Qt::CaseInsensitive);
    plan.repositoryRoot = resolveProductRoot(environment.applicationDirectory);
    if (plan.repositoryRoot.isEmpty())
        plan.repositoryRoot = resolveProductRoot(QDir::currentPath());

    if (package.operations.contains(QStringLiteral("check_compatibility"))) {
        const auto compatibility =
            invokeFrontend(software->family, QStringLiteral("check_compatibility"),
                           {{"softwareArchitecture", software->architecture},
                            {"softwareVersion", software->version},
                            {"integrationAdapterId", application->integrationAdapterId},
                            {"assignmentMode", assignmentModeKey(application->assignmentMode)},
                            {"channelCount", application->assignmentMode == AssignmentMode::FullAccess ? model.effectiveMappingsForApplication(applicationId).size() : application->channelCount},
                            {"language", int(language)},
                            {"applicationDirectory", environment.applicationDirectory},
                            {"inspectLaunchAssets", environment.inspectLaunchAssets},
                            {"plan", plan.toJson()}});
        if (!compatibility.ok())
            appendBlocker(&plan, compatibility.error);
        else {
            plan.frontend = compatibility.value.value("frontend").toString(package.name);
            for (const auto &b : compatibility.value.value("blockers").toArray())
                appendBlocker(&plan, b.toString());
        }
    } else
        plan.frontend = package.name;

    plan.ready = plan.blockers.isEmpty();
    return plan;
}

}  // namespace Ucal::Manager
