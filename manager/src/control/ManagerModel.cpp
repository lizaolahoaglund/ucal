#include "ManagerModel.h"
#include "PortablePaths.h"
#include "FrontendPlugins.h"
#include "SoftwareDiscovery.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace Ucal::Manager {
QVector<int> duplicateEndpointChannels(const QVector<ChannelMapping>& mappings,
                                      const int channelCount) {
    QMap<int, QString> active;
    for (const auto& mapping : mappings)
        if (mapping.logicalChannel > 0 && mapping.logicalChannel <= channelCount &&
            !active.contains(mapping.logicalChannel)) active.insert(mapping.logicalChannel, mapping.endpointId);
    QMap<QString, int> counts;
    for (const auto& endpoint : active)
        if (!endpoint.isEmpty()) ++counts[endpoint];
    QVector<int> conflicts;
    for (auto mapping = active.cbegin(); mapping != active.cend(); ++mapping)
        if (!mapping.value().isEmpty() && counts.value(mapping.value()) > 1)
            conflicts.push_back(mapping.key());
    return conflicts;
}
namespace {

constexpr int kSchemaVersion = 3;
constexpr int kLegacySchemaVersion = 1;
constexpr int kMinimumChannels = 1;
constexpr int kPhaseOneMaximumChannels = 16;

QJsonObject mappingToJson(const ChannelMapping& mapping) {
    QJsonObject object;
    object.insert(QStringLiteral("bus"), QStringLiteral("CAN"));
    object.insert(QStringLiteral("logicalChannel"), mapping.logicalChannel);
    object.insert(QStringLiteral("endpointId"), mapping.endpointId);
    return object;
}

ChannelMapping mappingFromJson(const QJsonObject& object) {
    return {object.value(QStringLiteral("logicalChannel")).toInt(),
            object.value(QStringLiteral("endpointId")).toString()};
}

QJsonObject endpointToJson(const Endpoint& endpoint) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), endpoint.id);
    object.insert(QStringLiteral("adapterId"), endpoint.adapterId);
    object.insert(QStringLiteral("backendFamily"), endpoint.backendFamily);
    object.insert(QStringLiteral("backendInstanceId"), endpoint.backendInstanceId);
    object.insert(QStringLiteral("backendName"), endpoint.backendName);
    object.insert(QStringLiteral("vendor"), endpoint.vendor);
    object.insert(QStringLiteral("model"), endpoint.model);
    object.insert(QStringLiteral("serial"), endpoint.serial);
    object.insert(QStringLiteral("busType"), endpoint.busType);
    object.insert(QStringLiteral("physicalChannel"), endpoint.physicalChannel);
    object.insert(QStringLiteral("firmwareVersion"), endpoint.firmwareVersion);
    object.insert(QStringLiteral("capabilities"), endpoint.capabilities);
    object.insert(QStringLiteral("maturity"), endpoint.maturity);
    object.insert(QStringLiteral("compatibilityIssue"), endpoint.compatibilityIssue);

    object.insert(QStringLiteral("physicalIndex"), QString::number(endpoint.physicalIndex));
    object.insert(QStringLiteral("physicalIndexValid"), endpoint.physicalIndexValid);
    return object;
}

bool endpointFromJson(const QJsonObject& object, Endpoint* endpoint) {
    if (endpoint == nullptr || !object.value(QStringLiteral("id")).isString()) return false;
    endpoint->id = object.value(QStringLiteral("id")).toString();
    endpoint->adapterId = object.value(QStringLiteral("adapterId")).toString();
    endpoint->backendFamily = object.value(QStringLiteral("backendFamily")).toString();
    endpoint->backendInstanceId = object.value(QStringLiteral("backendInstanceId")).toString();
    endpoint->backendName = object.value(QStringLiteral("backendName")).toString();
    endpoint->vendor = object.value(QStringLiteral("vendor")).toString();
    endpoint->model = object.value(QStringLiteral("model")).toString();
    endpoint->serial = object.value(QStringLiteral("serial")).toString();
    endpoint->busType = object.value(QStringLiteral("busType")).toString();
    endpoint->physicalChannel = object.value(QStringLiteral("physicalChannel")).toInt();
    endpoint->firmwareVersion = object.value(QStringLiteral("firmwareVersion")).toString();
    endpoint->capabilities = object.value(QStringLiteral("capabilities")).toString();
    endpoint->maturity = object.value(QStringLiteral("maturity")).toString();
    endpoint->compatibilityIssue = object.value(QStringLiteral("compatibilityIssue")).toString();
    endpoint->state = endpointStateFromKey(object.value(QStringLiteral("state")).toString());
    bool physicalIndexOk = false;
    const QJsonValue rawPhysicalIndex = object.value(QStringLiteral("physicalIndex"));
    const qulonglong parsedPhysicalIndex = rawPhysicalIndex.isString()
        ? rawPhysicalIndex.toString().toULongLong(&physicalIndexOk)
        : static_cast<qulonglong>(rawPhysicalIndex.toDouble(-1.0));
    if (!rawPhysicalIndex.isString()) {
        physicalIndexOk = rawPhysicalIndex.isDouble() && parsedPhysicalIndex <= 0xffffffffull;
    }
    endpoint->physicalIndexValid = object.value(QStringLiteral("physicalIndexValid")).toBool(false) &&
                                   physicalIndexOk && parsedPhysicalIndex <= 0xffffffffull;
    endpoint->physicalIndex = endpoint->physicalIndexValid
        ? static_cast<quint32>(parsedPhysicalIndex) : 0u;
    if (endpoint->adapterId.isEmpty()) endpoint->adapterId = endpoint->id.section(':', 0, 2);
    return !endpoint->id.isEmpty();
}

QJsonObject softwareToJson(const SoftwareInstallation& software) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), software.id);
    object.insert(QStringLiteral("family"), software.family);
    object.insert(QStringLiteral("displayName"), software.displayName);
    object.insert(QStringLiteral("version"), software.version);
    object.insert(QStringLiteral("architecture"), software.architecture);
    object.insert(QStringLiteral("executablePath"), software.executablePath);
    object.insert(QStringLiteral("source"), software.source);
    object.insert(QStringLiteral("compatibility"), software.compatibility);
    object.insert(QStringLiteral("compatibilityKind"), software.compatibilityKind);
    object.insert(QStringLiteral("available"), software.available);
    object.insert(QStringLiteral("integrationAdapters"),
                  QJsonArray::fromStringList(software.integrationAdapters));
    return object;
}

SoftwareInstallation softwareFromJson(const QJsonObject& object) {
    SoftwareInstallation software;
    software.id = object.value(QStringLiteral("id")).toString();
    software.family = object.value(QStringLiteral("family")).toString();
    software.displayName = object.value(QStringLiteral("displayName")).toString();
    software.version = object.value(QStringLiteral("version")).toString();
    software.architecture = object.value(QStringLiteral("architecture")).toString();
    software.executablePath = object.value(QStringLiteral("executablePath")).toString();
    software.source = object.value(QStringLiteral("source")).toString();
    software.compatibility = object.value(QStringLiteral("compatibility")).toString();
    software.compatibilityKind = object.value(QStringLiteral("compatibilityKind")).toString();
    software.available = object.value(QStringLiteral("available")).toBool(true);
    for (const QJsonValue& value : object.value(QStringLiteral("integrationAdapters")).toArray()) {
        software.integrationAdapters.push_back(value.toString());
    }
    return software;
}

QJsonObject applicationToJson(const ApplicationProfile& application) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), application.id);
    object.insert(QStringLiteral("name"), application.name);
    object.insert(QStringLiteral("softwareInstallationId"), application.softwareInstallationId);
    object.insert(QStringLiteral("integrationAdapterId"), application.integrationAdapterId);
    object.insert(QStringLiteral("integrationName"), application.integrationName);
    object.insert(QStringLiteral("assignmentMode"), assignmentModeKey(application.assignmentMode));
    object.insert(QStringLiteral("channelCount"), application.channelCount);
    object.insert(QStringLiteral("maturity"), application.maturity);
    object.insert(QStringLiteral("statusKind"), application.statusKind);
    object.insert(QStringLiteral("shortcutCreated"), application.shortcutCreated);
    object.insert(QStringLiteral("configurationPath"), application.configurationPath);
    object.insert(QStringLiteral("commandLineArguments"), application.commandLineArguments);
    object.insert(QStringLiteral("createdAtUtc"), application.createdAtUtc);
    QJsonArray mappings;
    for (const ChannelMapping& mapping : application.mappings) mappings.append(mappingToJson(mapping));
    object.insert(QStringLiteral("mappings"), mappings);
    return object;
}

ApplicationProfile applicationFromJson(const QJsonObject& object) {
    ApplicationProfile application;
    application.id = object.value(QStringLiteral("id")).toString();
    application.name = object.value(QStringLiteral("name")).toString();
    application.softwareInstallationId = object.value(QStringLiteral("softwareInstallationId")).toString();
    application.integrationAdapterId = object.value(QStringLiteral("integrationAdapterId")).toString();
    application.integrationName = object.value(QStringLiteral("integrationName")).toString();
    application.assignmentMode = assignmentModeFromKey(object.value(QStringLiteral("assignmentMode")).toString());
    application.channelCount = object.value(QStringLiteral("channelCount")).toInt(4);
    application.maturity = object.value(QStringLiteral("maturity")).toString();
    application.statusKind = object.value(QStringLiteral("statusKind")).toString();
    application.shortcutCreated = object.value(QStringLiteral("shortcutCreated")).toBool(false);
    application.configurationPath = object.value(QStringLiteral("configurationPath")).toString();
    application.commandLineArguments = object.value(QStringLiteral("commandLineArguments")).toString();
    application.createdAtUtc = object.value(QStringLiteral("createdAtUtc")).toString();
    for (const QJsonValue& value : object.value(QStringLiteral("mappings")).toArray()) {
        if (value.isObject()) application.mappings.push_back(mappingFromJson(value.toObject()));
    }
    return application;
}

QJsonObject projectToJson(const Project& project) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), project.id);
    object.insert(QStringLiteral("name"), project.name);
    object.insert(QStringLiteral("includeAllApplications"), project.includeAllApplications);
    object.insert(QStringLiteral("applicationIds"), QJsonArray::fromStringList(project.applicationIds));
    return object;
}

Project projectFromJson(const QJsonObject& object) {
    Project project;
    project.id = object.value(QStringLiteral("id")).toString();
    project.name = object.value(QStringLiteral("name")).toString();
    project.includeAllApplications = object.value(QStringLiteral("includeAllApplications")).toBool(false);
    for (const QJsonValue& value : object.value(QStringLiteral("applicationIds")).toArray()) {
        project.applicationIds.push_back(value.toString());
    }
    return project;
}

}  // namespace

QString endpointStateText(const EndpointState state) {
    switch (state) {
    case EndpointState::Online: return QStringLiteral("在线");
    case EndpointState::Offline: return QStringLiteral("离线 / 已移除");
    case EndpointState::Busy: return QStringLiteral("忙碌");
    case EndpointState::DriverMissing: return QStringLiteral("驱动缺失");
    case EndpointState::Incompatible: return QStringLiteral("不兼容");
    case EndpointState::Disabled: return QStringLiteral("驱动已禁用");
    }
    return QStringLiteral("未知");
}

QString endpointStateKey(const EndpointState state) {
    switch (state) {
    case EndpointState::Online: return QStringLiteral("online");
    case EndpointState::Busy: return QStringLiteral("busy");
    case EndpointState::DriverMissing: return QStringLiteral("driver-missing");
    case EndpointState::Incompatible: return QStringLiteral("incompatible");
    case EndpointState::Offline: return QStringLiteral("offline");
    case EndpointState::Disabled: return QStringLiteral("disabled");
    }
    return QStringLiteral("offline");
}

EndpointState endpointStateFromKey(const QString& key) {
    if (key == QStringLiteral("disabled")) return EndpointState::Disabled;
    if (key == QStringLiteral("online")) return EndpointState::Online;
    if (key == QStringLiteral("busy")) return EndpointState::Busy;
    if (key == QStringLiteral("driver-missing")) return EndpointState::DriverMissing;
    if (key == QStringLiteral("incompatible")) return EndpointState::Incompatible;
    return EndpointState::Offline;
}

QString assignmentModeText(const AssignmentMode mode) {
    return mode == AssignmentMode::FullAccess ? QStringLiteral("全部兼容通道")
                                              : QStringLiteral("指定通道");
}

QString assignmentModeKey(const AssignmentMode mode) {
    return mode == AssignmentMode::FullAccess ? QStringLiteral("full-access")
                                              : QStringLiteral("selected");
}

AssignmentMode assignmentModeFromKey(const QString& key) {
    return key == QStringLiteral("selected") ? AssignmentMode::Selected
                                             : AssignmentMode::FullAccess;
}

ManagerModel ManagerModel::reviewFixture(const QString& name) {
    QFile file(QStringLiteral(":/ucal/review/") + name + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly)) qFatal("Missing UCAL review fixture");
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    ManagerModel model;
    model.primaryApplicationId_ = object.value(QStringLiteral("primary_application_id")).toString();
    for (const auto& value : object.value(QStringLiteral("endpoints")).toArray()) {
        Endpoint endpoint;
        if (!endpointFromJson(value.toObject(), &endpoint)) qFatal("Invalid UCAL review endpoint");
        endpoint.state = endpointStateFromKey(value.toObject().value(QStringLiteral("state")).toString());
        model.endpoints_.push_back(endpoint);
    }
    for (const auto& value : object.value(QStringLiteral("softwareInstallations")).toArray())
        model.softwareInstallations_.push_back(softwareFromJson(value.toObject()));
    for (const auto& value : object.value(QStringLiteral("applications")).toArray())
        model.applications_.push_back(applicationFromJson(value.toObject()));
    for (const auto& value : object.value(QStringLiteral("projects")).toArray())
        model.projects_.push_back(projectFromJson(value.toObject()));
    for (const auto& value : object.value(QStringLiteral("backendPackages")).toArray()) {
        const auto backend = value.toObject();
        model.backendPackages_.push_back({backend.value("id").toString(), backend.value("name").toString(),
            backend.value("vendor").toString(), backend.value("version").toString(),
            backend.value("architecture").toString(), backend.value("source").toString(),
            backend.value("maturity").toString(), backend.value("state").toString(),
            backend.value("capabilities").toString(), backend.value("builtIn").toBool(),
            backend.value("enabled").toBool()});
    }
    model.activeProjectId_ = object.value(QStringLiteral("activeProjectId")).toString();
    if (const auto* primary = model.primaryApplication()) {
        model.channelCount_ = primary->channelCount;
        model.mappings_ = primary->mappings;
    }
    return model;
}

ManagerModel ManagerModel::demo() { return reviewFixture(QStringLiteral("demo")); }
ManagerModel ManagerModel::realisticDemo() { return reviewFixture(QStringLiteral("realistic")); }

ManagerModel ManagerModel::productionDefaults() {
    ManagerModel model;
    model.projects_ = {
        {QStringLiteral("default"), QStringLiteral("Default project"), true, {}},
    };
    model.activeProjectId_ = QStringLiteral("default");
    return model;
}

int ManagerModel::clampedChannelCount(const int count) {
    return std::max(kMinimumChannels, std::min(count, kPhaseOneMaximumChannels));
}

ApplicationProfile* ManagerModel::primaryApplication() {
    auto found = std::find_if(applications_.begin(), applications_.end(), [this](const ApplicationProfile& app) { return app.id == primaryApplicationId_; });
    if (found != applications_.end()) return &(*found);
    return applications_.isEmpty() ? nullptr : &applications_.front();
}

const ApplicationProfile* ManagerModel::primaryApplication() const {
    auto found = std::find_if(applications_.cbegin(), applications_.cend(), [this](const ApplicationProfile& app) { return app.id == primaryApplicationId_; });
    if (found != applications_.cend()) return &(*found);
    return applications_.isEmpty() ? nullptr : &applications_.front();
}

int ManagerModel::channelCount() const { return channelCount_; }

void ManagerModel::setChannelCount(const int count) {
    channelCount_ = clampedChannelCount(count);
    for (int channel = 1; channel <= channelCount_; ++channel) {
        const auto found = std::find_if(mappings_.cbegin(), mappings_.cend(), [channel](const ChannelMapping& mapping) { return mapping.logicalChannel == channel; });
        if (found == mappings_.cend()) mappings_.push_back({channel, QString()});
    }
    if (ApplicationProfile* application = primaryApplication()) {
        application->channelCount = channelCount_;
        application->mappings = mappings_;
    }
}

const QVector<Endpoint>& ManagerModel::endpoints() const { return endpoints_; }
const QVector<SoftwareInstallation>& ManagerModel::softwareInstallations() const { return softwareInstallations_; }
const QVector<ApplicationProfile>& ManagerModel::applications() const { return applications_; }
const QVector<Project>& ManagerModel::projects() const { return projects_; }
const QVector<BackendPackage>& ManagerModel::backendPackages() const { return backendPackages_; }

int SoftwareInstallation::maximumCanChannels() const {
    // Public frontend profiles have different channel limits. Keep the UI and
    // explicit channel edits on the same capability boundary.
    const auto package = frontendPackage(family);
    return package.id.isEmpty() ? kPhaseOneMaximumChannels : package.maximumCanChannels;
}

void ManagerModel::reconcileBackendPackages(const QVector<BackendPackage>& packages) {
    for (auto& existing : backendPackages_) existing.manifestAvailable = false;
    for (const auto& package : packages) {
        auto found = std::find_if(backendPackages_.begin(), backendPackages_.end(),
            [&](const BackendPackage& existing) { return existing.id == package.id; });
        if (found == backendPackages_.end()) backendPackages_.push_back(package);
        else *found = package;
    }
}

void ManagerModel::replaceBackendPackages(const QVector<BackendPackage>& packages) {
    backendPackages_ = packages;
}

QVector<ChannelMapping> ManagerModel::visibleMappings() const {
    QVector<ChannelMapping> result;
    for (int channel = 1; channel <= channelCount_; ++channel) result.push_back({channel, endpointIdForChannel(channel)});
    return result;
}

const Endpoint* ManagerModel::endpointById(const QString& endpointId) const {
    const auto found = std::find_if(endpoints_.cbegin(), endpoints_.cend(), [&endpointId](const Endpoint& endpoint) { return endpoint.id == endpointId; });
    return found == endpoints_.cend() ? nullptr : &(*found);
}

const SoftwareInstallation* ManagerModel::softwareById(const QString& installationId) const {
    const auto found = std::find_if(softwareInstallations_.cbegin(), softwareInstallations_.cend(), [&installationId](const SoftwareInstallation& item) { return item.id == installationId; });
    return found == softwareInstallations_.cend() ? nullptr : &(*found);
}

const ApplicationProfile* ManagerModel::applicationById(const QString& applicationId) const {
    const auto found = std::find_if(applications_.cbegin(), applications_.cend(), [&applicationId](const ApplicationProfile& item) { return item.id == applicationId; });
    return found == applications_.cend() ? nullptr : &(*found);
}

ApplicationProfile* ManagerModel::mutableApplicationById(const QString& applicationId) {
    const auto found = std::find_if(applications_.begin(), applications_.end(), [&applicationId](const ApplicationProfile& item) { return item.id == applicationId; });
    return found == applications_.end() ? nullptr : &(*found);
}

const Project* ManagerModel::projectById(const QString& projectId) const {
    const auto found = std::find_if(projects_.cbegin(), projects_.cend(), [&projectId](const Project& item) { return item.id == projectId; });
    return found == projects_.cend() ? nullptr : &(*found);
}

const BackendPackage* ManagerModel::backendPackageById(const QString& packageId) const {
    const auto found = std::find_if(backendPackages_.cbegin(), backendPackages_.cend(),
                                    [&packageId](const BackendPackage& item) {
        return item.id == packageId;
    });
    return found == backendPackages_.cend() ? nullptr : &(*found);
}

QVector<ApplicationProfile> ManagerModel::applicationsForProject(const QString& projectId) const {
    const Project* project = projectById(projectId);
    if (project == nullptr || project->includeAllApplications) return applications_;
    QVector<ApplicationProfile> result;
    for (const QString& applicationId : project->applicationIds) {
        if (const ApplicationProfile* application = applicationById(applicationId)) result.push_back(*application);
    }
    return result;
}

QString ManagerModel::activeProjectId() const { return activeProjectId_; }

bool ManagerModel::setActiveProject(const QString& projectId) {
    if (projectById(projectId) == nullptr) return false;
    activeProjectId_ = projectId;
    return true;
}

bool ManagerModel::addProject(const QString& name, QString* createdId) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return false;
    const QString id = QStringLiteral("project-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    projects_.push_back({id, trimmed, false, {}});
    if (createdId != nullptr) *createdId = id;
    return true;
}

bool ManagerModel::renameProject(const QString& projectId, const QString& name) {
    if (projectId == QStringLiteral("default") || name.trimmed().isEmpty()) return false;
    for (Project& project : projects_) {
        if (project.id == projectId) { project.name = name.trimmed(); return true; }
    }
    return false;
}

bool ManagerModel::removeProject(const QString& projectId) {
    if (projectId == QStringLiteral("default")) return false;
    const int oldSize = projects_.size();
    projects_.erase(std::remove_if(projects_.begin(), projects_.end(), [&projectId](const Project& project) { return project.id == projectId; }), projects_.end());
    if (projects_.size() == oldSize) return false;
    if (activeProjectId_ == projectId) activeProjectId_ = QStringLiteral("default");
    return true;
}

bool ManagerModel::setProjectApplications(const QString& projectId, const QStringList& applicationIds) {
    if (projectId == QStringLiteral("default")) return false;
    for (const QString& id : applicationIds) if (applicationById(id) == nullptr) return false;
    for (Project& project : projects_) {
        if (project.id == projectId) { project.includeAllApplications = false; project.applicationIds = applicationIds; project.applicationIds.removeDuplicates(); return true; }
    }
    return false;
}

bool ManagerModel::setApplicationAssignmentMode(const QString& applicationId, const AssignmentMode mode) {
    ApplicationProfile* application = mutableApplicationById(applicationId);
    if (application == nullptr) return false;
    application->assignmentMode = mode;
    return true;
}

bool ManagerModel::setApplicationChannelCount(const QString& applicationId, const int count) {
    ApplicationProfile* application = mutableApplicationById(applicationId);
    if (application == nullptr) return false;
    application->channelCount = clampedChannelCount(count);
    for (int channel = 1; channel <= application->channelCount; ++channel) {
        const auto found = std::find_if(application->mappings.cbegin(), application->mappings.cend(), [channel](const ChannelMapping& mapping) { return mapping.logicalChannel == channel; });
        if (found == application->mappings.cend()) application->mappings.push_back({channel, QString()});
    }
    if (application == primaryApplication()) { channelCount_ = application->channelCount; mappings_ = application->mappings; }
    return true;
}

bool ManagerModel::replaceApplicationChannels(const QString& applicationId,
                                              const QVector<QString>& endpointIds) {
    auto* application = mutableApplicationById(applicationId);
    if (application == nullptr) return false;
    const auto* software = softwareById(application->softwareInstallationId);
    const int limit = software == nullptr ? kPhaseOneMaximumChannels : software->maximumCanChannels();
    if (endpointIds.isEmpty() || endpointIds.size() > limit) return false;
    for (const auto& id : endpointIds)
        if (!id.isEmpty() && endpointById(id) == nullptr) return false;
    QVector<ChannelMapping> mappings;
    for (int i = 0; i < endpointIds.size(); ++i) mappings.push_back({i + 1, endpointIds.at(i)});
    if (!duplicateEndpointChannels(mappings, endpointIds.size()).isEmpty()) return false;
    application->channelCount = endpointIds.size();
    application->mappings = mappings;
    application->assignmentMode = AssignmentMode::Selected;
    if (application == primaryApplication()) {
        channelCount_ = application->channelCount;
        mappings_ = mappings;
    }
    return true;
}

bool ManagerModel::setApplicationMapping(const QString& applicationId, const int logicalChannel, const QString& endpointId) {
    ApplicationProfile* application = mutableApplicationById(applicationId);
    if (application == nullptr || logicalChannel < kMinimumChannels || logicalChannel > kPhaseOneMaximumChannels) return false;
    if (!endpointId.isEmpty() && endpointById(endpointId) == nullptr) return false;
    if (!endpointId.isEmpty())
        for (const auto& existing : application->mappings)
            if (existing.logicalChannel != logicalChannel && existing.endpointId == endpointId)
                return false;
    auto found = std::find_if(application->mappings.begin(), application->mappings.end(), [logicalChannel](const ChannelMapping& mapping) { return mapping.logicalChannel == logicalChannel; });
    if (found == application->mappings.end()) application->mappings.push_back({logicalChannel, endpointId});
    else found->endpointId = endpointId;
    application->assignmentMode = AssignmentMode::Selected;
    if (application == primaryApplication()) { mappings_ = application->mappings; channelCount_ = application->channelCount; }
    return true;
}

QVector<ChannelMapping> ManagerModel::fullAccessMappings() const {
    QVector<const Endpoint*> compatible;
    for (const Endpoint& endpoint : endpoints_) {
        if (endpoint.state == EndpointState::Online && backendEnabledForEndpoint(endpoint) &&
            endpoint.busType.compare(QStringLiteral("CAN"), Qt::CaseInsensitive) == 0) {
            compatible.push_back(&endpoint);
        }
    }
    std::sort(compatible.begin(), compatible.end(), [](const Endpoint* left,
                                                        const Endpoint* right) {
        if (left->adapterId != right->adapterId) return left->adapterId < right->adapterId;
        if (left->physicalIndexValid != right->physicalIndexValid) {
            return left->physicalIndexValid;
        }
        if (left->physicalIndex != right->physicalIndex) {
            return left->physicalIndex < right->physicalIndex;
        }
        return left->id < right->id;
    });
    QVector<ChannelMapping> result;
    int logicalChannel = 1;
    for (const Endpoint* endpoint : compatible) {
        result.push_back({logicalChannel++, endpoint->id});
    }
    return result;
}

QVector<ChannelMapping> ManagerModel::effectiveMappingsForApplication(
    const QString& applicationId) const {
    const ApplicationProfile* application = applicationById(applicationId);
    if (application == nullptr) return {};
    if (application->assignmentMode == AssignmentMode::FullAccess) {
        const auto* software = softwareById(application->softwareInstallationId);
        return fullAccessMappings().mid(0, software == nullptr ? kPhaseOneMaximumChannels : software->maximumCanChannels());
    }

    QVector<ChannelMapping> result;
    for (int channel = 1; channel <= application->channelCount; ++channel) {
        const auto found = std::find_if(
            application->mappings.cbegin(), application->mappings.cend(),
            [channel](const ChannelMapping& mapping) {
                return mapping.logicalChannel == channel;
            });
        result.push_back(found == application->mappings.cend()
            ? ChannelMapping{channel, QString()}
            : *found);
    }
    return result;
}

bool ManagerModel::setApplicationEndpointAssigned(const QString& applicationId,
                                                 const QString& endpointId,
                                                 const bool assigned) {
    ApplicationProfile* application = mutableApplicationById(applicationId);
    if (application == nullptr || endpointById(endpointId) == nullptr ||
        application->assignmentMode == AssignmentMode::FullAccess) {
        return false;
    }
    QVector<int> matchingChannels;
    for (const ChannelMapping& mapping : application->mappings) {
        if (mapping.logicalChannel > 0 &&
            mapping.logicalChannel <= application->channelCount &&
            mapping.endpointId == endpointId) {
            matchingChannels.push_back(mapping.logicalChannel);
        }
    }
    if (!assigned) {
        for (const int channel : matchingChannels) {
            if (!setApplicationMapping(applicationId, channel, QString())) return false;
        }
        return true;
    }
    if (!matchingChannels.isEmpty()) return true;

    const auto* software = softwareById(application->softwareInstallationId);
    const int limit = software == nullptr ? kPhaseOneMaximumChannels : software->maximumCanChannels();
    if (application->channelCount > limit) return false;

    int targetChannel = 0;
    for (int channel = 1; channel <= application->channelCount; ++channel) {
        const auto mapping = std::find_if(
            application->mappings.cbegin(), application->mappings.cend(),
            [channel](const ChannelMapping& item) { return item.logicalChannel == channel; });
        if (mapping == application->mappings.cend() || mapping->endpointId.isEmpty()) {
            targetChannel = channel;
            break;
        }
    }
    if (targetChannel == 0) {
        if (application->channelCount >= limit) return false;
        targetChannel = application->channelCount + 1;
        if (!setApplicationChannelCount(applicationId, targetChannel)) return false;
    }
    return setApplicationMapping(applicationId, targetChannel, endpointId);
}

bool ManagerModel::addApplication(const QString& softwareInstallationId, const QString& name, QString* createdId) {
    const SoftwareInstallation* software = softwareById(softwareInstallationId);
    if (software == nullptr || name.trimmed().isEmpty() || !software->available ||
        software->compatibilityKind == QStringLiteral("danger") ||
        !std::any_of(software->integrationAdapters.cbegin(), software->integrationAdapters.cend(),
                     [](const QString& adapter) { return adapter != QStringLiteral("native"); })) return false;
    const QString id = QStringLiteral("application-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString adapter = software->integrationAdapters.isEmpty() ? QStringLiteral("unassigned") : software->integrationAdapters.first();
    applications_.push_back({id, name.trimmed(), softwareInstallationId, adapter,
                             QStringLiteral("Select integration method"),
                             AssignmentMode::FullAccess, 0, {},
                             QStringLiteral("Preflight required"),
                             QStringLiteral("warning"), false});
    if (!frontendPackage(software->family).id.isEmpty()) {
        // Begin with an explicit unbound slot. The user chooses the hardware;
        // discovery must not silently bind every endpoint to a new profile.
        applications_.last().assignmentMode = AssignmentMode::Selected;
        applications_.last().channelCount = 1;
        applications_.last().mappings = {{1, QString()}};
    }
    applications_.last().createdAtUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for (Project& project : projects_) {
        if (project.id == activeProjectId_ && !project.includeAllApplications) project.applicationIds.push_back(id);
    }
    if (createdId != nullptr) *createdId = id;
    return true;
}

bool ManagerModel::removeApplication(const QString& applicationId) {
    const int oldSize = applications_.size();
    applications_.erase(std::remove_if(applications_.begin(), applications_.end(), [&applicationId](const ApplicationProfile& application) { return application.id == applicationId; }), applications_.end());
    if (applications_.size() == oldSize) return false;
    for (Project& project : projects_) project.applicationIds.removeAll(applicationId);
    if (const ApplicationProfile* primary = primaryApplication()) { channelCount_ = primary->channelCount; mappings_ = primary->mappings; }
    return true;
}

QString ManagerModel::addManualSoftware(const QString& executablePath, QString* error) {
    if (error) error->clear();
    const QFileInfo info(executablePath);
    if (!info.isFile()) return QString();
    const QString normalized = QDir::cleanPath(info.absoluteFilePath());
    const auto existing = std::find_if(
        softwareInstallations_.cbegin(), softwareInstallations_.cend(),
        [&normalized](const SoftwareInstallation& software) {
            return QDir::cleanPath(QFileInfo(software.executablePath).absoluteFilePath())
                       .compare(normalized, Qt::CaseInsensitive) == 0;
        });
    if (existing != softwareInstallations_.cend()) return existing->id;

    SoftwareInstallation inspected =
        inspectSoftwareExecutable(normalized, QStringLiteral("Manual"), error);
    if (inspected.id.isEmpty()) return QString();
    if (inspected.family != QStringLiteral("custom")) rememberSoftwareSearchPath(inspected.executablePath);
    softwareInstallations_.push_back(inspected);
    return inspected.id;
}

bool ManagerModel::reinspectManualSoftware(const QString& softwareInstallationId, QString* error) {
    if (error) error->clear();
    const auto found = std::find_if(
        softwareInstallations_.begin(), softwareInstallations_.end(),
        [&softwareInstallationId](const SoftwareInstallation& software) {
            return software.id == softwareInstallationId;
        });
    if (found == softwareInstallations_.end() ||
        (found->source != QStringLiteral("Manual") &&
         found->source != QStringLiteral("手动添加"))) {
        return false;
    }
    SoftwareInstallation inspected =
        inspectSoftwareExecutable(found->executablePath, QStringLiteral("Manual"), error);
    if (error && !error->isEmpty()) return false;
    if (inspected.id.isEmpty()) {
        found->available = false;
        found->compatibilityKind = QStringLiteral("danger");
        found->compatibility =
            QStringLiteral("Unsupported: the executable is missing or unreadable");
        found->integrationAdapters.clear();
        return true;
    }
    inspected.id = found->id;
    *found = inspected;
    return true;
}

QStringList ManagerModel::applicationNamesUsingSoftware(
    const QString& softwareInstallationId) const {
    QStringList names;
    for (const ApplicationProfile& application : applications_) {
        if (application.softwareInstallationId == softwareInstallationId) {
            names.push_back(application.name);
        }
    }
    return names;
}

bool ManagerModel::removeManualSoftware(const QString& softwareInstallationId) {
    if (!applicationNamesUsingSoftware(softwareInstallationId).isEmpty()) return false;
    const auto found = std::find_if(
        softwareInstallations_.begin(), softwareInstallations_.end(),
        [&softwareInstallationId](const SoftwareInstallation& software) {
            return software.id == softwareInstallationId;
        });
    if (found == softwareInstallations_.end() ||
        (found->source != QStringLiteral("Manual") &&
         found->source != QStringLiteral("手动添加"))) {
        return false;
    }
    softwareInstallations_.erase(found);
    return true;
}

QString ManagerModel::endpointIdForChannel(const int logicalChannel) const {
    const auto found = std::find_if(mappings_.cbegin(), mappings_.cend(), [logicalChannel](const ChannelMapping& mapping) { return mapping.logicalChannel == logicalChannel; });
    return found == mappings_.cend() ? QString() : found->endpointId;
}

void ManagerModel::setMapping(const int logicalChannel, const QString& endpointId) {
    if (const auto* application = primaryApplication()) {
        setApplicationMapping(application->id, logicalChannel, endpointId);
        return;
    }
    if (logicalChannel < kMinimumChannels || logicalChannel > kPhaseOneMaximumChannels) return;
    if (!endpointId.isEmpty() && endpointById(endpointId) == nullptr) return;
    auto found = std::find_if(mappings_.begin(), mappings_.end(), [logicalChannel](const ChannelMapping& mapping) { return mapping.logicalChannel == logicalChannel; });
    if (found == mappings_.end()) mappings_.push_back({logicalChannel, endpointId});
    else found->endpointId = endpointId;
    if (ApplicationProfile* application = primaryApplication()) { application->assignmentMode = AssignmentMode::Selected; application->mappings = mappings_; }
}

const BackendPackage* ManagerModel::backendPackageForEndpoint(const Endpoint& endpoint) const {
    if (!endpoint.backendInstanceId.isEmpty()) return backendPackageById(endpoint.backendInstanceId);
    const auto normalized = [](QString value) { return value.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9]"))).toLower(); };
    const BackendPackage* match = nullptr;
    for (const auto& package : backendPackages_) {
        if (normalized(package.backendFamily) != normalized(endpoint.backendFamily)) continue;
        if (match != nullptr) return nullptr; // A legacy family cannot select one of several instances.
        match = &package;
    }
    return match;
}

QString ManagerModel::backendDisplayName(const Endpoint& endpoint) const {
    const auto* package = backendPackageForEndpoint(endpoint);
    return package ? package->name : endpoint.backendName.isEmpty() ? endpoint.backendFamily : endpoint.backendName;
}

bool ManagerModel::backendEnabledForEndpoint(const Endpoint& endpoint) const {
    const auto* package = backendPackageForEndpoint(endpoint);
    return package == nullptr || package->enabled;
}

bool ManagerModel::setBackendPackageEnabled(const QString& packageId, const bool enabled) {
    const auto package = std::find_if(backendPackages_.begin(), backendPackages_.end(), [&](const auto& p) { return p.id == packageId; });
    if (package == backendPackages_.end()) return false;
    package->enabled = enabled;
    for (auto& endpoint : endpoints_) {
        if (!backendEnabledForEndpoint(endpoint)) endpoint.state = EndpointState::Disabled;
        else if (endpoint.state == EndpointState::Disabled) endpoint.state = EndpointState::Offline;
    }
    return true;
}

void ManagerModel::refreshRuntimeStateFrom(const ManagerModel& current) {
    endpoints_ = current.endpoints_;
    observedThisSession_ = current.observedThisSession_;
    backendPackages_ = current.backendPackages_;
    softwareInstallations_ = current.softwareInstallations_;
}

void ManagerModel::reconcileInventory(const QVector<Endpoint>& liveEndpoints) {
    QSet<QString> bound;
    for (const auto& app : applications_) for (const auto& mapping : app.mappings)
        if (!mapping.endpointId.isEmpty()) bound.insert(mapping.endpointId);
    QVector<Endpoint> next;
    QSet<QString> observed;
    for (auto existing : endpoints_) {
        const auto live = std::find_if(liveEndpoints.cbegin(), liveEndpoints.cend(), [&](const auto& e) { return e.id == existing.id; });
        if (live != liveEndpoints.cend()) {
            existing = *live; existing.historicalBinding = false;
            observed.insert(existing.id); observedThisSession_.insert(existing.id); next << existing;
        } else if (bound.contains(existing.id) || observedThisSession_.contains(existing.id)) {
            existing.state = EndpointState::Offline;
            existing.historicalBinding = !observedThisSession_.contains(existing.id);
            next << existing;
        }
    }
    for (auto endpoint : liveEndpoints) if (!observed.contains(endpoint.id)) {
        endpoint.historicalBinding = false; next << endpoint; observedThisSession_.insert(endpoint.id);
    }
    endpoints_ = std::move(next);
    for (auto& endpoint : endpoints_) if (!backendEnabledForEndpoint(endpoint)) endpoint.state = EndpointState::Disabled;
}

void ManagerModel::reconcileSoftwareInstallations(
    const QVector<SoftwareInstallation>& liveInstallations) {
    QVector<QString> observed;
    for (SoftwareInstallation& existing : softwareInstallations_) {
        const auto live = std::find_if(
            liveInstallations.cbegin(), liveInstallations.cend(),
            [&existing](const SoftwareInstallation& installation) {
                return installation.id == existing.id;
            });
        if (live != liveInstallations.cend()) {
            existing = *live;
            observed.push_back(live->id);
        } else if (existing.source == QStringLiteral("Manual") ||
                   existing.source == QStringLiteral("手动添加")) {
            const QString retainedId = existing.id;
            SoftwareInstallation inspected =
                inspectSoftwareExecutable(existing.executablePath,
                                          QStringLiteral("Manual"));
            if (!inspected.id.isEmpty()) {
                inspected.id = retainedId;
                existing = inspected;
            } else {
                existing.available = false;
                existing.compatibilityKind = QStringLiteral("danger");
                existing.compatibility =
                    QStringLiteral("Unsupported: the executable is missing or unreadable");
                existing.integrationAdapters.clear();
            }
        } else {
            existing.available = false;
        }
    }
    for (const SoftwareInstallation& installation : liveInstallations) {
        if (!observed.contains(installation.id) && softwareById(installation.id) == nullptr) {
            softwareInstallations_.push_back(installation);
        }
    }
}

QJsonObject ManagerModel::toJson() const {
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), kSchemaVersion);
    root.insert(QStringLiteral("activeProjectId"), activeProjectId_);
    QJsonArray endpoints;
    QSet<QString> endpointReferences;
    QSet<QString> softwareReferences;
    for (const auto& app : applications_) {
        softwareReferences.insert(app.softwareInstallationId);
        for (const auto& mapping : app.mappings) endpointReferences.insert(mapping.endpointId);
    }
    for (const auto& endpoint : endpoints_) if (endpointReferences.contains(endpoint.id)) endpoints.append(endpointToJson(endpoint));
    root.insert(QStringLiteral("endpointBindings"), endpoints);
    QJsonArray software;
    for (const auto& installation : softwareInstallations_) if (softwareReferences.contains(installation.id)) {
        auto binding = softwareToJson(installation); binding.remove(QStringLiteral("available")); software.append(binding);
    }
    root.insert(QStringLiteral("softwareBindings"), software);
    QJsonArray applications;
    for (const ApplicationProfile& application : applications_) applications.append(applicationToJson(application));
    root.insert(QStringLiteral("applications"), applications);
    QJsonArray projects;
    for (const Project& project : projects_) projects.append(projectToJson(project));
    root.insert(QStringLiteral("projects"), projects);
    return root;
}

bool ManagerModel::fromJson(const QJsonObject& object, ManagerModel* model, QString* error) {
    if (model == nullptr) return false;
    const int schemaVersion = object.value(QStringLiteral("schemaVersion")).toInt(-1);
    if (schemaVersion == kLegacySchemaVersion) {
        ManagerModel migrated = ManagerModel::demo();
        QVector<Endpoint> endpoints;
        for (const QJsonValue& value : object.value(QStringLiteral("inventoryCache")).toArray()) {
            Endpoint endpoint;
            if (value.isObject() && endpointFromJson(value.toObject(), &endpoint)) endpoints.push_back(endpoint);
        }
        if (!endpoints.isEmpty()) migrated.endpoints_ = endpoints;
        ApplicationProfile* primary = migrated.primaryApplication();
        if (primary != nullptr) {
            primary->channelCount = clampedChannelCount(object.value(QStringLiteral("channelCount")).toInt(4));
            primary->mappings.clear();
            for (const QJsonValue& value : object.value(QStringLiteral("mappings")).toArray()) if (value.isObject()) primary->mappings.push_back(mappingFromJson(value.toObject()));
            migrated.channelCount_ = primary->channelCount;
            migrated.mappings_ = primary->mappings;
        }
        migrated.reconcileInventory({});
        for (auto& endpoint : migrated.endpoints_) endpoint.historicalBinding = true;
        for (auto& software : migrated.softwareInstallations_) software.available = false;
        *model = migrated;
        return true;
    }
    if (schemaVersion != kSchemaVersion && schemaVersion != 2) {
        if (error != nullptr) *error = QStringLiteral("不支持的配置 schemaVersion");
        return false;
    }
    ManagerModel parsed;
    for (const auto& value : object.value(schemaVersion == 3 ? QStringLiteral("endpointBindings") : QStringLiteral("inventoryCache")).toArray()) {
        Endpoint endpoint;
        if (!value.isObject() || !endpointFromJson(value.toObject(), &endpoint)) {
            if (error != nullptr) *error = QStringLiteral("配置中存在无效端点");
            return false;
        }
        parsed.endpoints_.push_back(endpoint);
    }
    for (const auto& value : object.value(schemaVersion == 3 ? QStringLiteral("softwareBindings") : QStringLiteral("softwareInstallations")).toArray()) if (value.isObject()) parsed.softwareInstallations_.push_back(softwareFromJson(value.toObject()));
    for (const QJsonValue& value : object.value(QStringLiteral("applications")).toArray()) if (value.isObject()) parsed.applications_.push_back(applicationFromJson(value.toObject()));
    for (const QJsonValue& value : object.value(QStringLiteral("projects")).toArray()) if (value.isObject()) parsed.projects_.push_back(projectFromJson(value.toObject()));
    for (auto& project : parsed.projects_) {
        if (project.id == QStringLiteral("default")) { project.includeAllApplications = true; continue; }
        if (!project.includeAllApplications) continue;
        // Custom projects store explicit membership. Older all-app projects keep
        // their current members, then remain editable without acquiring new apps.
        project.includeAllApplications = false;
        project.applicationIds.clear();
        for (const auto& app : parsed.applications_) project.applicationIds << app.id;
    }
    if (parsed.projectById(QStringLiteral("default")) == nullptr) parsed.projects_.prepend({QStringLiteral("default"), QStringLiteral("默认项目"), true, {}});
    parsed.activeProjectId_ = object.value(QStringLiteral("activeProjectId")).toString(QStringLiteral("default"));
    if (parsed.projectById(parsed.activeProjectId_) == nullptr) parsed.activeProjectId_ = QStringLiteral("default");
    if (const ApplicationProfile* primary = parsed.primaryApplication()) { parsed.channelCount_ = primary->channelCount; parsed.mappings_ = primary->mappings; }
    parsed.reconcileInventory({});
    for (auto& endpoint : parsed.endpoints_) { endpoint.state = EndpointState::Offline; endpoint.historicalBinding = true; }
    parsed.softwareInstallations_.erase(std::remove_if(parsed.softwareInstallations_.begin(), parsed.softwareInstallations_.end(), [&](const auto& software) {
        return std::none_of(parsed.applications_.cbegin(), parsed.applications_.cend(), [&](const auto& app) { return app.softwareInstallationId == software.id; });
    }), parsed.softwareInstallations_.end());
    for (auto& software : parsed.softwareInstallations_) software.available = false;
    *model = parsed;
    return true;
}

bool ManagerModel::saveToFile(const QString& path, QString* error) const {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error != nullptr) *error = QStringLiteral("无法创建配置目录：%1").arg(info.absolutePath());
        return false;
    }
    if (QFile::exists(path)) {
        const QString backupPath = path + QStringLiteral(".bak");
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly)) {
            if (error != nullptr) *error = source.errorString();
            return false;
        }
        const QByteArray original = source.readAll();
        if (source.error() != QFile::NoError) {
            if (error != nullptr) *error = source.errorString();
            return false;
        }
        QSaveFile backup(backupPath);
        if (!backup.open(QIODevice::WriteOnly) || backup.write(original) != original.size() ||
            !backup.commit()) {
            if (error != nullptr) *error = backup.errorString();
            return false;
        }
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const QByteArray contents = QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    return true;
}

bool ManagerModel::loadFromFile(const QString& path, ManagerModel* model, QString* error) {
    enum class LoadStatus { Loaded, RecoverableFailure, UnsupportedSchema };
    const auto loadCandidate = [](const QString& candidatePath, ManagerModel* candidateModel,
                                  QString* candidateError) {
        QFile file(candidatePath);
        if (!file.open(QIODevice::ReadOnly)) {
            if (candidateError != nullptr) *candidateError = file.errorString();
            return LoadStatus::RecoverableFailure;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (candidateError != nullptr) *candidateError = parseError.errorString();
            return LoadStatus::RecoverableFailure;
        }
        const int schemaVersion = document.object().value(QStringLiteral("schemaVersion")).toInt(-1);
        if (schemaVersion != kSchemaVersion && schemaVersion != 2 && schemaVersion != kLegacySchemaVersion) {
            ManagerModel::fromJson(document.object(), candidateModel, candidateError);
            return LoadStatus::UnsupportedSchema;
        }
        if (!ManagerModel::fromJson(document.object(), candidateModel, candidateError)) {
            return LoadStatus::RecoverableFailure;
        }
        return LoadStatus::Loaded;
    };

    QString primaryError;
    const LoadStatus primaryStatus = loadCandidate(path, model, &primaryError);
    if (primaryStatus == LoadStatus::Loaded) {
        if (error != nullptr) error->clear();
        return true;
    }
    if (primaryStatus == LoadStatus::UnsupportedSchema) {
        if (error != nullptr) *error = primaryError;
        return false;
    }

    const QString backupPath = path + QStringLiteral(".bak");
    QString backupError;
    if (QFile::exists(backupPath) &&
        loadCandidate(backupPath, model, &backupError) == LoadStatus::Loaded) {
        if (error != nullptr) error->clear();
        return true;
    }
    if (error != nullptr) {
        *error = primaryError;
        if (!backupError.isEmpty()) {
            *error += QStringLiteral("; backup: ") + backupError;
        }
    }
    return false;
}

QString ManagerModel::defaultStoragePath() {
    return QDir(portableDataDirectory())
        .filePath(QStringLiteral("profiles-poc.json"));
}

QString ManagerModel::productionStoragePath() {
    return QDir(portableDataDirectory())
        .filePath(QStringLiteral("profiles.json"));
}

}  // namespace Ucal::Manager
