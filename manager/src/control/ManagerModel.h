#pragma once
#include "PackageTool.h"
#include <QMap>
#include <QSet>

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>
#include "ucal/Model.hpp"

namespace Ucal::Manager {

enum class EndpointState {
    Online,
    Offline,
    Busy,
    DriverMissing,
    Incompatible,
    Disabled,
};

QString endpointStateText(EndpointState state);
QString endpointStateKey(EndpointState state);
EndpointState endpointStateFromKey(const QString& key);

struct Endpoint {
    QString id;
    QString adapterId;
    QString backendFamily;
    QString vendor;
    QString model;
    QString serial;
    QString busType;
    int physicalChannel = 0;
    QString firmwareVersion;
    QString capabilities;
    QString maturity;
    EndpointState state = EndpointState::Offline;
    // The Broker's exact raw physical_index. Keep this separate from the
    // human-facing one-based channel number: some public runtimes expose a
    // stable channel handle (for example 0x51) rather than zero-based ports.
    quint32 physicalIndex = 0;
    bool physicalIndexValid = false;
    QString compatibilityIssue = QString();
    Ucal::CanConfigurationCapabilities configuration{};
    bool bindingIsDefault = false;
    QString backendInstanceId;
    QString backendName;
    bool historicalBinding = false;
};

struct ChannelMapping {
    int logicalChannel = 1;
    QString endpointId;
};

// Returns every logical slot involved in a duplicate binding, in slot order.
// Empty and inactive mappings are ignored so unfinished profiles can be saved.
QVector<int> duplicateEndpointChannels(const QVector<ChannelMapping>& mappings,
                                      int channelCount);

enum class AssignmentMode {
    FullAccess,
    Selected,
};

QString assignmentModeText(AssignmentMode mode);
QString assignmentModeKey(AssignmentMode mode);
AssignmentMode assignmentModeFromKey(const QString& key);

struct SoftwareInstallation {
    QString id;
    QString family;
    QString displayName;
    QString version;
    QString architecture;
    QString executablePath;
    QString source;
    QString compatibility;
    QString compatibilityKind;
    QStringList integrationAdapters;
    bool available = true;
    int maximumCanChannels() const;
};

struct ApplicationProfile {
    QString id;
    QString name;
    QString softwareInstallationId;
    QString integrationAdapterId;
    QString integrationName;
    AssignmentMode assignmentMode = AssignmentMode::FullAccess;
    int channelCount = 4;
    QVector<ChannelMapping> mappings;
    QString maturity;
    QString statusKind;
    bool shortcutCreated = false;
    QString configurationPath = QString();
    QString commandLineArguments = QString();
    QString createdAtUtc = QString();
};

struct Project {
    QString id;
    QString name;
    bool includeAllApplications = false;
    QStringList applicationIds;
};

struct BackendChannelBinding {
    QString identity;
    QString name;
    QString selector;
    quint32 physicalPort = 0;
    bool brs = true;
    bool termination = false;
    QString adapterIdentity;
};

struct BackendPackage {
    BackendPackage() = default;
    BackendPackage(const QString& packageId, const QString& packageName,
                   const QString& packageVendor, const QString& packageVersion,
                   const QString& packageArchitecture, const QString& packageSource,
                   const QString& packageMaturity, const QString& packageState,
                   const QString& packageCapabilities, bool packageBuiltIn,
                   bool packageEnabled)
        : id(packageId), name(packageName), vendor(packageVendor), version(packageVersion),
          architecture(packageArchitecture), source(packageSource), maturity(packageMaturity),
          state(packageState), capabilities(packageCapabilities), builtIn(packageBuiltIn),
          enabled(packageEnabled) {}

    QString id;
    QString name;
    QString vendor;
    QString version;
    QString architecture;
    QString source;
    QString maturity;
    QString state;
    QString capabilities;
    bool builtIn = true;
    bool enabled = true;
    QString backendFamily;
    QString manifestPath;
    QJsonValue description;
    bool manifestAvailable = true;
    QString mode;
    QString type;
    bool preserveEndpointIdentity = false;
    bool packageOwnsChannels = false;
    bool runtimeDiscovery = false;
    QString discoveryLibraryPath;
    QString configListLayout;
    QString managerVersion;
    PackageTool configurationTool;
    QString libraryPath;
    QString configurationPath;
    QString configurationTemplatePath;
    QString adapterPath;
    QString workerPath;
    QString loadError;
    QVector<BackendChannelBinding> defaultChannels;
    QMap<QString, QString> fixedTiming;
    quint32 pins = 1550;
};

class ManagerModel {
public:
    static ManagerModel demo();
    static ManagerModel realisticDemo();
    static ManagerModel productionDefaults();

    int channelCount() const;
    void setChannelCount(int count);

    const QVector<Endpoint>& endpoints() const;
    QVector<ChannelMapping> visibleMappings() const;

    const QVector<SoftwareInstallation>& softwareInstallations() const;
    const QVector<ApplicationProfile>& applications() const;
    const QVector<Project>& projects() const;
    const QVector<BackendPackage>& backendPackages() const;

    const SoftwareInstallation* softwareById(const QString& installationId) const;
    const ApplicationProfile* applicationById(const QString& applicationId) const;
    ApplicationProfile* mutableApplicationById(const QString& applicationId);
    const Project* projectById(const QString& projectId) const;
    const BackendPackage* backendPackageById(const QString& packageId) const;
    QVector<ApplicationProfile> applicationsForProject(const QString& projectId) const;

    QString activeProjectId() const;
    bool setActiveProject(const QString& projectId);
    bool addProject(const QString& name, QString* createdId = nullptr);
    bool renameProject(const QString& projectId, const QString& name);
    bool removeProject(const QString& projectId);
    bool setProjectApplications(const QString& projectId, const QStringList& applicationIds);

    bool setApplicationAssignmentMode(const QString& applicationId, AssignmentMode mode);
    bool setApplicationChannelCount(const QString& applicationId, int count);
    // Explicit user edit: replace rows and renumber only this application's slots.
    bool replaceApplicationChannels(const QString& applicationId,
                                    const QVector<QString>& endpointIds);
    bool setApplicationMapping(const QString& applicationId, int logicalChannel,
                               const QString& endpointId);
    QVector<ChannelMapping> fullAccessMappings() const;
    QVector<ChannelMapping> effectiveMappingsForApplication(
        const QString& applicationId) const;
    bool setApplicationEndpointAssigned(const QString& applicationId,
                                        const QString& endpointId, bool assigned);
    bool addApplication(const QString& softwareInstallationId, const QString& name,
                        QString* createdId = nullptr);
    bool removeApplication(const QString& applicationId);
    QString addManualSoftware(const QString& executablePath, QString* error = nullptr);
    bool reinspectManualSoftware(const QString& softwareInstallationId, QString* error = nullptr);
    bool removeManualSoftware(const QString& softwareInstallationId);
    QStringList applicationNamesUsingSoftware(const QString& softwareInstallationId) const;
    void replaceBackendPackages(const QVector<BackendPackage>& packages);
    void reconcileBackendPackages(const QVector<BackendPackage>& packages);

    const Endpoint* endpointById(const QString& endpointId) const;
    QString endpointIdForChannel(int logicalChannel) const;
    void setMapping(int logicalChannel, const QString& endpointId);

    void reconcileInventory(const QVector<Endpoint>& liveEndpoints);
    // Merge current runtime catalogs without replacing a staged configuration edit.
    void refreshRuntimeStateFrom(const ManagerModel& current);
    QString backendDisplayName(const Endpoint& endpoint) const;
    const BackendPackage* backendPackageForEndpoint(const Endpoint& endpoint) const;
    bool backendEnabledForEndpoint(const Endpoint& endpoint) const;
    bool setBackendPackageEnabled(const QString& packageId, bool enabled);
    void reconcileSoftwareInstallations(const QVector<SoftwareInstallation>& liveInstallations);

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& object, ManagerModel* model, QString* error = nullptr);

    bool saveToFile(const QString& path, QString* error = nullptr) const;
    static bool loadFromFile(const QString& path, ManagerModel* model, QString* error = nullptr);
    static QString defaultStoragePath();
    static QString productionStoragePath();

private:
    static ManagerModel reviewFixture(const QString& name);
    static int clampedChannelCount(int count);
    ApplicationProfile* primaryApplication();
    const ApplicationProfile* primaryApplication() const;

    int channelCount_ = 4;
    QString primaryApplicationId_;
    QVector<Endpoint> endpoints_;
    QSet<QString> observedThisSession_;
    QVector<ChannelMapping> mappings_;
    QVector<SoftwareInstallation> softwareInstallations_;
    QVector<ApplicationProfile> applications_;
    QVector<Project> projects_;
    QVector<BackendPackage> backendPackages_;
    QString activeProjectId_ = QStringLiteral("default");
};

}  // namespace Ucal::Manager
