#include "RuntimeDiscovery.h"
#include "control/BackendPlugins.h"
#include "control/PluginMetadata.h"
#include "control/SoftwareDiscovery.h"

#include "ucal/Client.hpp"

#include <QStringList>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include "control/ProductOperations.h"
#include <QRegularExpression>

#include <vector>

namespace Ucal::Manager {
namespace {

QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QString statusText(const Ucal::Status status) {
    switch (status) {
    case Ucal::Status::Ok: return QStringLiteral("ok");
    case Ucal::Status::InvalidArgument: return QStringLiteral("invalid argument");
    case Ucal::Status::IncompatibleVersion: return QStringLiteral("incompatible version");
    case Ucal::Status::Unsupported: return QStringLiteral("unsupported");
    case Ucal::Status::NotFound: return QStringLiteral("not found");
    case Ucal::Status::Busy: return QStringLiteral("busy");
    case Ucal::Status::AccessDenied: return QStringLiteral("access denied");
    case Ucal::Status::TransportError: return QStringLiteral("transport error");
    case Ucal::Status::InternalError: return QStringLiteral("internal error");
    }
    return QStringLiteral("unknown status %1").arg(static_cast<int>(status));
}

QString capabilitiesText(const std::uint32_t capabilities) {
    QStringList values;
    const auto add = [&values, capabilities](const Ucal::Capability capability,
                                             const char* label) {
        if ((capabilities & static_cast<std::uint32_t>(capability)) != 0) {
            values.push_back(QString::fromLatin1(label));
        }
    };
    add(Ucal::CapabilityClassicCan, "CAN");
    add(Ucal::CapabilityCanFdIso, "CAN FD");
    add(Ucal::CapabilityBrs, "BRS");
    add(Ucal::CapabilityLin, "LIN");
    add(Ucal::CapabilityLinMaster, "LIN master");
    add(Ucal::CapabilityLinSlave, "LIN slave");
    add(Ucal::CapabilityListenOnly, "Listen-only");
    add(Ucal::CapabilityTermination, "120 ohm termination");
    add(Ucal::CapabilityNoAck, "No-ACK");
    add(Ucal::CapabilityTxCompletion, "Tx completion");
    if ((capabilities & Ucal::CapabilityDiscoveryOnly) != 0) {
        values.push_back(QStringLiteral("Discovery only"));
    }
    return values.isEmpty() ? QStringLiteral("No capabilities reported")
                            : values.join(QStringLiteral(" / "));
}

EndpointState endpointState(const Ucal::DeviceState state) {
    switch (state) {
    case Ucal::DeviceState::Available: return EndpointState::Online;
    case Ucal::DeviceState::Leased: return EndpointState::Busy;
    case Ucal::DeviceState::Offline: return EndpointState::Offline;
    }
    return EndpointState::Offline;
}

struct BackendIdentity {
    QString family, vendor, fallbackDescription;
    int handleFirst = -1, handleLast = -1, handleOffset = 0;
};

QMap<QString, BackendIdentity> backendIdentities(const QVector<BackendPackage>& packages) {
    QMap<QString, BackendIdentity> result;
    QMap<QString, int> familyCounts;
    for (const auto& package : packages) ++familyCounts[package.backendFamily.toLower()];
    for (const auto& package : packages) {
        QFile file(package.manifestPath);
        if (!file.open(QIODevice::ReadOnly)) continue;
        QJsonValue normalized;
        if (!convertPluginJson(QJsonDocument::fromJson(file.readAll()).object(), &normalized, false)) continue;
        const auto display = normalized.toObject().value("endpointPresentation").toObject();
        BackendIdentity identity;
        identity.family = display.value("family").toString(package.backendFamily);
        identity.vendor = package.vendor;
        identity.handleFirst = display.value("handleFirst").toInt(-1);
        identity.handleLast = display.value("handleLast").toInt(-1);
        identity.handleOffset = display.value("handleOffset").toInt();
        identity.fallbackDescription = display.value("fallbackDescription").toString();
        result.insert(QStringLiteral("id:") + package.id, identity);
        if (familyCounts.value(package.backendFamily.toLower()) == 1)
            result.insert(QStringLiteral("family:") + package.backendFamily.toLower(), identity);
    }
    return result;
}

BackendIdentity backendIdentity(const Ucal::EndpointDescriptor& descriptor,
                                 const QMap<QString, BackendIdentity>& identities) {
    const auto family = fromUtf8(descriptor.backendFamily);
    const auto instanceKey = QStringLiteral("id:") + fromUtf8(descriptor.backendInstanceId);
    if (identities.contains(instanceKey)) return identities.value(instanceKey);
    const auto familyKey = QStringLiteral("family:") + family.toLower();
    if (identities.contains(familyKey)) return identities.value(familyKey);
    if (family == QStringLiteral("j2534")) return {QStringLiteral("J2534"), QStringLiteral("Multi-vendor"), {}};
    if (family == QStringLiteral("mock")) return {QStringLiteral("UCAL mock"), QStringLiteral("UCAL"), {}};
    return {family, {}, {}};
}

bool usesChannelHandle(const BackendIdentity& backend, const Ucal::EndpointDescriptor& descriptor) {
    return backend.handleFirst >= 0 && descriptor.physicalIndex >= quint32(backend.handleFirst) &&
           descriptor.physicalIndex <= quint32(backend.handleLast);
}

int displayChannel(const BackendIdentity& backend, const Ucal::EndpointDescriptor& descriptor) {
    return usesChannelHandle(backend, descriptor)
        ? int(descriptor.physicalIndex) - backend.handleOffset : int(descriptor.physicalIndex + 1u);
}

Endpoint projectEndpoint(const Ucal::EndpointDescriptor& descriptor,
                         const QMap<QString, BackendIdentity>& identities) {
    const BackendIdentity backend = backendIdentity(descriptor, identities);
    Endpoint endpoint;
    endpoint.id = QString::fromStdString(Ucal::deviceIdToString(descriptor.id));
    endpoint.adapterId = QString::fromStdString(Ucal::deviceIdToString(descriptor.adapterId));
    endpoint.backendFamily = backend.family;
    endpoint.vendor = backend.vendor;
    endpoint.model = fromUtf8(descriptor.name);
    if (endpoint.model.isEmpty()) endpoint.model = fromUtf8(descriptor.model);
    endpoint.model.remove(QRegularExpression(QStringLiteral("\\s+CH\\d+\\b.*$")));
    endpoint.model.remove(QRegularExpression(QStringLiteral("\\s+Channel\\s+\\d+$"), QRegularExpression::CaseInsensitiveOption));
    endpoint.serial = fromUtf8(descriptor.serial);
    if (endpoint.serial.isEmpty()) endpoint.serial = QStringLiteral("Not reported");
    endpoint.busType = descriptor.busType == Ucal::BusType::Lin
        ? QStringLiteral("LIN") : QStringLiteral("CAN");
    endpoint.physicalChannel = displayChannel(backend, descriptor);
    endpoint.firmwareVersion = QStringLiteral("Not reported by Broker");
    endpoint.capabilities = capabilitiesText(descriptor.capabilities);
    endpoint.configuration = descriptor.configuration;
    endpoint.bindingIsDefault = descriptor.bindingIsDefault;
    endpoint.backendInstanceId = fromUtf8(descriptor.backendInstanceId);
    endpoint.maturity = QStringLiteral("Live Broker inventory");
    if (usesChannelHandle(backend, descriptor) && !backend.fallbackDescription.isEmpty())
        endpoint.maturity = backend.fallbackDescription;
    endpoint.state = endpointState(descriptor.state);
    if (!Ucal::endpointMeetsDeviceContract(descriptor)) {
        endpoint.state = EndpointState::Incompatible;
        endpoint.compatibilityIssue = QStringLiteral("iso-can-fd-required");
    }
    endpoint.physicalIndex = descriptor.physicalIndex;
    endpoint.physicalIndexValid = true;
    return endpoint;
}

}  // namespace

QVector<Endpoint> projectRuntimeEndpoints(const std::vector<Ucal::EndpointDescriptor>& descriptors,
                                         const QVector<BackendPackage>& packages) {
    const auto identities = backendIdentities(packages);
    QVector<Endpoint> endpoints;
    endpoints.reserve(static_cast<int>(descriptors.size()));
    for (const auto& descriptor : descriptors) endpoints.push_back(projectEndpoint(descriptor, identities));
    return endpoints;
}

RuntimeDiscoveryResult discoverRuntimeEndpoints(const bool scanSoftware) {
    RuntimeDiscoveryResult result;
    result.softwareInventoryScanned = scanSoftware;
    if (scanSoftware) result.softwareInstallations = discoverInstalledSoftware(&result.softwareDiagnostics);

    const Ucal::Client client;
    const Ucal::Status ping = client.ping();
    if (ping != Ucal::Status::Ok) {
        result.error = QStringLiteral("UCAL service unavailable: %1").arg(statusText(ping));
        return result;
    }

    if(QFileInfo::exists(backendPluginCatalogPath())){
        const auto configured=client.configureBackendPlugins(backendPluginCatalogPath().toUtf8().toStdString());
        if(configured!=Ucal::Status::Ok)result.backendWarnings<<QStringLiteral("Backend plugin catalog: %1; a matching service version is required").arg(statusText(configured));
    }
    result.brokerAvailable = true;
    std::vector<Ucal::EndpointDescriptor> descriptors;
    const Ucal::Status status = client.enumerateEndpoints(descriptors);
    if (status != Ucal::Status::Ok) {
        result.error = QStringLiteral("Endpoint enumeration failed: %1").arg(statusText(status));
        return result;
    }
    std::vector<Ucal::BackendHealth> backendHealth;
    const Ucal::Status healthStatus = client.enumerateBackendHealth(backendHealth);
    if (healthStatus != Ucal::Status::Ok) {
        result.error = QStringLiteral("Backend health enumeration failed: %1")
                           .arg(statusText(healthStatus));
        return result;
    }
    for (const Ucal::BackendHealth& health : backendHealth) {
        const QString family = fromUtf8(health.backendInstanceId.empty()?health.backendFamily:health.backendInstanceId);
        if (!health.runtime.path.empty()) {
            result.backendRuntimes.insert(family, {fromUtf8(health.runtime.path),
                fromUtf8(health.runtime.fileVersion), fromUtf8(health.runtime.apiVersion),
                fromUtf8(health.runtime.sha256)});
        }
        result.backendHealth.push_back(
            QStringLiteral("%1: %2 (%3 endpoints)")
                .arg(family, statusText(health.status))
                .arg(health.endpointCount));
        if (health.status != Ucal::Status::Ok) {
            result.backendWarnings.push_back(
                QStringLiteral("%1 backend: %2").arg(family, statusText(health.status)));
        }
    }
    const auto packages = scanBackendDirectory(backendDirectory(QCoreApplication::applicationDirPath())).packages;
    result.endpoints = projectRuntimeEndpoints(descriptors, packages);
    for (const Ucal::EndpointDescriptor& descriptor : descriptors) {
        if (!Ucal::endpointMeetsDeviceContract(descriptor)) {
            result.backendWarnings.push_back(QStringLiteral("%1: incompatible CAN port; ISO CAN FD is required")
                .arg(fromUtf8(descriptor.backendFamily)));
        }
    }
    return result;
}

}  // namespace Ucal::Manager
