#pragma once

#include "ManagerModel.h"
#include "UiPresentation.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

namespace Ucal::Manager {

struct LaunchEndpoint {
    int logicalChannel = 0;
    QString endpointId;
    QString adapterId;
    QString backendFamily;
    QString serial;
    QString busType;
    quint32 physicalIndex = 0;
    int displayChannel = 1;

    QJsonObject toJson() const;
};

struct LaunchEnvironment {
    QString applicationDirectory;
    bool inspectLaunchAssets = true;
    UiLanguage language = UiLanguage::English;
};

struct LaunchPlan {
    QString frontend;
    QString sessionId;
    QString createdUtc;
    QString applicationId;
    QString applicationName;
    QString softwareInstallationId;
    QString softwareFamily;
    QString softwareVersion;
    QString softwareArchitecture;
    QString softwareExecutable;
    QString configurationPath;
    QString commandLineArguments;
    QString integrationAdapterId;
    AssignmentMode assignmentMode = AssignmentMode::FullAccess;
    bool nativeLaunch = false;
    bool brokerInventoryFresh = false;
    bool ready = false;
    QVector<LaunchEndpoint> endpoints;
    QStringList backendFamilies;
    QStringList blockers;
    QString repositoryRoot;

    QJsonObject toJson() const;
    QString summary(UiLanguage language = UiLanguage::English) const;
};

class LaunchCoordinator {
public:
    static LaunchPlan prepare(const ManagerModel& model, const QString& applicationId,
                              bool brokerInventoryFresh, bool nativeLaunch,
                              const LaunchEnvironment& environment = {});
};

}  // namespace Ucal::Manager
