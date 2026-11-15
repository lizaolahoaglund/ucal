#pragma once


#include "control/ManagerModel.h"
#include "ucal/Model.hpp"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <vector>

namespace Ucal::Manager {

struct RuntimeLibraryDetails {
    QString path;
    QString fileVersion;
    QString apiVersion;
    QString sha256;
};

struct RuntimeDiscoveryResult {
    bool brokerAvailable = false;
    bool softwareInventoryScanned = false;
    QVector<Endpoint> endpoints;
    QVector<SoftwareInstallation> softwareInstallations;
    QStringList softwareDiagnostics;
    QStringList backendHealth;
    QStringList backendWarnings;
    QMap<QString, RuntimeLibraryDetails> backendRuntimes;
    QString error;
};

RuntimeDiscoveryResult discoverRuntimeEndpoints(bool scanSoftware = true);

// Pure projection of live inventory; package metadata supplies presentation only.
QVector<Endpoint> projectRuntimeEndpoints(const std::vector<Ucal::EndpointDescriptor>& descriptors,
                                         const QVector<BackendPackage>& packages);

}  // namespace Ucal::Manager
