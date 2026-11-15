#include "frontends/pcanview/src/PcanViewText.h"
#include "frontends/canoe/src/CanoeText.h"
#include "../src/control/ManagerModel.h"
#include "frontends/canoe/src/DeploymentAssets.h"
#include "frontends/canoe/src/LaunchCopy.h"
#include "frontends/canoe/src/PeForwarder.h"
#include "../src/control/PluginMetadata.h"
#include "Branding.h"
#include "ucal/FrontendPlugin.h"
#include <QLibrary>
#include "HardwareChangeMonitor.h"
#include "frontends/pcanview/src/Profile.h"
#include "../src/control/BackendPlugins.h"
#include "../src/control/PortablePaths.h"
#include "RuntimeDiscovery.h"
#include "../src/control/FrontendPlugins.h"
#include "../src/control/PackageTool.h"

#include "../src/control/LaunchPlan.h"
#include "MainWindow.h"
#include "ProcessWatchdog.h"
#include "../src/control/ProductLauncher.h"
#include "../src/control/ProductOperations.h"
#include "SingleInstance.h"
#include "ShortcutWriter.h"
#include "../src/control/SoftwareDiscovery.h"
#include "../src/control/UiPresentation.h"
#include "../src/control/UserPreferences.h"

#include <algorithm>
#include <iostream>
#include <limits>

#include <QFile>
#include <QGuiApplication>
#include <QAction>
#include <QAbstractButton>
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QToolButton>
#include <QThread>
#include <QUuid>
#include <QtConcurrent>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#endif

using Ucal::Manager::Endpoint;
using Ucal::Manager::EndpointState;
using Ucal::Manager::AssignmentMode;
using Ucal::Manager::LaunchCoordinator;
using Ucal::Manager::ManagerModel;
using Ucal::Manager::PresentationMode;
using Ucal::Manager::UiLanguage;

namespace {

QString createFile(const QString& path, const QByteArray& contents = QByteArray("test")) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) return QString();
    file.close();
    return path;
}

void saveReviewSnapshot(QWidget* widget, const QString& name) {
    const auto directory = qEnvironmentVariable("UCAL_MANAGER_REVIEW_IMAGES");
    if (directory.isEmpty()) return;
    QDir().mkpath(directory);
    widget->grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

}  // namespace

namespace Ucal::Manager {
QString profileText(UiLanguage language, const QString& key) {
    return key.contains(QStringLiteral("pcan"), Qt::CaseInsensitive)
        ? pcanViewProfileText(language, key) : canoeProfileText(language, key);
}
}

namespace {
struct RegistryFixture {
    const QString root = QStringLiteral("HKEY_CURRENT_USER\\Software\\UCAL\\Tests\\") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    HKEY machineView = nullptr;
    bool overrideMachineView() {
        const auto path = root.mid(QStringLiteral("HKEY_CURRENT_USER\\").size());
        if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16()), 0,
                KEY_ALL_ACCESS | KEY_WOW64_64KEY, &machineView) != ERROR_SUCCESS) return false;
        if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, machineView) == ERROR_SUCCESS) return true;
        RegCloseKey(machineView);
        machineView = nullptr;
        return false;
    }
    ~RegistryFixture() {
        if (machineView) {
            if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr) != ERROR_SUCCESS)
                qFatal("Could not restore the test process registry view");
            RegCloseKey(machineView);
        }
        QSettings owned(root, QSettings::Registry64Format);
        owned.remove(QString());
        owned.sync();
    }
};
}

class ManagerModelTest final : public QObject {
    Q_OBJECT

private slots:
    void pluginMetadataPreservesOpaqueIntegrationIds();
    void frontendLaunchResourceSlotsFollowContract();
    void canoeForwardersPreserveLoadingContract();
    void canoeFirstLaunchCreatesOwnedCopy();
    void initTestCase();
    void coldStartupDoesNotWaitForBroker();
    void unavailableDeploymentActionsAreHidden();
    void compatibilityChecksRequiredLaunchAssets();
    void canoeDriverRegistrationPreflight();
    void duplicateEndpointMappingsAreRejectedAtomically();
    void duplicateLegacyEditorCanBeCorrected();
    void endpointChoicesPreventDuplicateBindings();
    void stagedEditorKeepsCurrentRuntimeState();
    void hardwareNotificationsCoalesceAndSettle();
    void windowsDeviceChangeReachesQtMonitor();
    void hardwareOnlyDiscoverySkipsSoftware();
    void launchEnvironmentSurvivesWindowsDriveVariables();
    void includeAllCustomProjectCanBeDeleted();
    void unsupportedSoftwareCannotCreateApplication();
    void pluginScanRestoresForgottenStandardInstallation();
    void portablePathsHonorIsolation();
    void softwareRescanRunsWithoutBroker();
    void pluginDescriptionsAndChannelBindings();
    void backendPackagesAreReadOnly();
    void backendEnablementPersistsWithoutChangingMetadata();
    void nativeBackendSettingsAreUserOwned();
    void userPackageRootDoesNotDependOnDeveloperTools();
    void hardwareKeywordFilterIsSessionOnly();
    void legacyCustomProjectMembershipIsEditable();
    void packageMetadataAndTools();
    void frontendPackagesAndExternalOperations();
    void phaseOneChannelCounts();
    void missingEndpointKeepsSlot();
    void reorderedInventoryDoesNotRemap();
    void roundTripsJsonAtomically();
    void retiredBusPresetIsIgnored();
    void corruptPrimaryRecoversFromBackup();
    void unsupportedFutureSchemaDoesNotRollback();
    void legacySchemaMigrates();
    void saveFailsClosedForDirectoryTarget();
    void saveFailsClosedWhenBackupCannotBeWritten();
    void saveFailsClosedForReadOnlyAndLockedTargets();
    void backupContainsExactPreviousGeneration();
    void corruptPrimaryAndBackupReportBothFailures();
    void backendFolderScanIsDeterministicAndFailClosed();
    void backendRescanPreservesMissingPackagesAndMappings();
    void explicitChannelEditsStayWithinSoftwareLimits();
    void legacyWorkspaceDoesNotRestoreDriverCatalog();
    void launchArgumentsRoundTripWithWindowsQuoting();
    void vxlProductAssetsAndBackupsAreBounded();
    void projectViewsDoNotOwnApplications();
    void multipleApplicationsCanShareSoftware();
    void assignmentModesStayExplicit();
    void endpointAssignmentUsesFreeSlotsWithoutOverwrite();
    void manualSoftwareCreatesAnIndependentInstallation();
    void manualSoftwareCompatibilityUsesExistingRoutes();
    void realisticDemoUsesProductFacingStates();
    void productionDefaultsContainNoDemoInventory();
    void softwareReconciliationPreservesMissingInstallation();
    void pcanViewVersionProfilesFailClosed();
    void translationCatalogSupportsRequiredLanguages();
    void startupWindowFitsAvailableDesktop();
    void presentationModeSupportsNamedArtifacts();
    void languagePreferenceRoundTrips();
    void userPreferencesRoundTrip();
    void startMinimizedPolicyIsBounded();
    void profileShortcutTargetsSwitchLauncher();
    void profileShortcutLaunchesWithWorkspace();
    void profileShortcutRoutesIntoProductInstance();
    void singleInstanceForwardsProfileByWorkspace();
    void singleInstanceModalRequestCanRepeat();
    void rebuiltChannelTableHasNoVisibleStaleEditors();
    void legacyChannelRowsRequireExplicitRemoval();
    void pcanDefaultsRequireAnExplicitEndpoint();
    void standardButtonsFollowUiLanguage();
    void productLauncherUsesDirectAcceptedRoutes();
    void canoeLaunchUsesShadowWhenPresent();
    void pcanViewConcurrentLaunchesUseUniqueSlots();
    void launchMessagesUseSelectedLanguage();
    void manualSoftwareRemovalHonorsReferences();
    void projectMutationsPreserveSharedApplications();
    void launchPlanUsesRawEndpointIdentity();
    void pcanViewLaunchPlanUsesExactEndpoint();
    void pcanViewLaunchPlanRejectsUnsupportedAssignments();
    void launchPlanRejectsNonUuidIdentity();
    void canoeLaunchPlanDoesNotRequireSerial();
    void launchPlanFailsClosedOnStaleInventory();
    void fullAccessLaunchUsesStableCatalogOrder();
    void pcanFullAccessEditorPersistsMode();
    void mainWindowNavigatesEveryPage();
    void reviewNavigationAndSoftwareUsage();
    void languageChangePreservesWindowAndProcesses();
    void startAllDispatchesEveryProjectProfile();
    void productStateLabelsRemainTruthful();
    void reviewDialogsExposeEveryWorkflowSurface();
    void keyboardControlsAreAccessibleAndOperable();
    void applicationWorkflowMutatesThroughWidgets();
    void projectWorkflowMutatesThroughWidgets();
    void hardwareUsageIsReadOnly();
    void runtimeInventoryIsNotRestoredAsOnline();
    void bridgeStopKeepsFailureVisible();
    void processWatchdogBoundsHungHelper();
    void settingsControlsPersistThroughWidgets();
    void askClosePolicyHonorsBothChoices();
    void trayCloseHidesAndOpenRestores();
    void closeWithoutTrayExitsNormally();
};


void ManagerModelTest::pluginMetadataPreservesOpaqueIntegrationIds() {
    const QJsonObject integrations{{"acme_route", QJsonObject{{"en", "Route A"}}},
        {"acmeRoute", "Route B"}, {"environment", "Route C"}};
    const QJsonObject environment{{"lowerCamelVariable", "a"}, {"lower_snake_variable", "b"}};
    const QJsonObject canonical{{"integrations", integrations}, {"integration_adapters", QJsonArray{"acme_route", "acmeRoute"}},
        {"environment", environment}, {"software_version", "12"}};
    QJsonValue legacy, roundTrip; QString error;
    QVERIFY(Ucal::Manager::convertPluginJson(canonical, &legacy, false, &error));
    QCOMPARE(legacy.toObject().value("integrations").toObject(), integrations);
    QCOMPARE(legacy.toObject().value("environment").toObject(), environment);
    QCOMPARE(legacy.toObject().value("integrationAdapters").toArray(), QJsonArray({"acme_route", "acmeRoute"}));
    QVERIFY(Ucal::Manager::convertPluginJson(legacy, &roundTrip, true, &error));
    QCOMPARE(roundTrip.toObject(), canonical);
    QVERIFY(!Ucal::Manager::convertPluginJson(QJsonObject{{"software_version", "12"}, {"softwareVersion", "13"}},
        &roundTrip, false, &error));
}

void ManagerModelTest::frontendLaunchResourceSlotsFollowContract() {
    QJsonObject description{{"program", QCoreApplication::applicationFilePath()},
        {"working_directory", QFileInfo(QCoreApplication::applicationFilePath()).absolutePath()},
        {"arguments", QJsonArray{}}, {"resource_slot", 7}};
    QJsonValue legacy; QString error;
    QVERIFY(Ucal::Manager::convertPluginJson(description, &legacy, false, &error));
    auto result = Ucal::Manager::productLaunchSpecFromJson(legacy.toObject());
    QVERIFY(result.valid()); QCOMPARE(result.resourceSlot, 7);
    description.remove("resource_slot"); description.insert("pcanNetIndex", 7);
    QVERIFY(Ucal::Manager::convertPluginJson(description, &legacy, false, &error));
    result = Ucal::Manager::productLaunchSpecFromJson(legacy.toObject());
    QVERIFY(result.valid()); QCOMPARE(result.resourceSlot, 7);
    description.insert("resource_slot", 8);
    QVERIFY(Ucal::Manager::convertPluginJson(description, &legacy, false, &error));
    QVERIFY(!Ucal::Manager::productLaunchSpecFromJson(legacy.toObject()).valid());
    description.remove("pcanNetIndex");
    for (const auto& value : {QJsonValue(-1), QJsonValue(17), QJsonValue(2.5), QJsonValue("7"), QJsonValue(QJsonValue::Null)}) {
        description.insert("resource_slot", value);
        QVERIFY(Ucal::Manager::convertPluginJson(description, &legacy, false, &error));
        QVERIFY(!Ucal::Manager::productLaunchSpecFromJson(legacy.toObject()).valid());
    }
    description.remove("resource_slot");
    QVERIFY(Ucal::Manager::convertPluginJson(description, &legacy, false, &error));
    result = Ucal::Manager::productLaunchSpecFromJson(legacy.toObject());
    QVERIFY(result.valid()); QCOMPARE(result.resourceSlot, 0);
}

void ManagerModelTest::canoeForwardersPreserveLoadingContract() {
    using namespace Ucal::Manager;
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto machine = quint16(sizeof(void*) == 8 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386);
    PeExportTable vendor; vendor.machine = machine; vendor.ordinalBase = 17;
    vendor.functions = {true, true, true, false, true, true};
    vendor.names = {{"xlOpenDriver", 0}, {"xlGetDriverConfigEx", 1}, {"VendorOnly", 2}, {"?Opaque@@YAHXZ", 4}};
    QMap<quint32, QByteArray> originalTargets;
    for (quint32 index : {0u, 1u, 2u, 4u, 5u}) originalTargets.insert(index, "KERNEL32.GetCurrentProcessId");
    PeExportTable data; data.machine = machine; data.ordinalBase = 1; data.functions = {true, true};
    data.names = {{"xlOpenDriver", 0}, {"xlGetDriverConfigEx", 1}};
    QString error;
    const auto origin = makePeForwarder(vendor, "OriginalVendor.dll", originalTargets, &error);
    const auto own = makePeForwarder(data, "OriginalData.dll", {{0,"KERNEL32.GetCurrentThreadId"}, {1,"KERNEL32.GetCurrentThreadId"}}, &error);
    QVERIFY2(!origin.isEmpty() && !own.isEmpty(), qPrintable(error));
    const auto shell = makeCanoeForwarder(origin, own, machine, "OriginalShell.dll", "OriginalVendor", "OriginalData", &error);
    QVERIFY2(!shell.isEmpty(), qPrintable(error));
    PeExportTable parsed; QVERIFY(readPeExports(shell, &parsed, &error));
    QCOMPARE(parsed.ordinalBase, vendor.ordinalBase);
    QCOMPARE(parsed.functions.size(), vendor.functions.size());
    for (int index = 0; index < vendor.functions.size(); ++index) QCOMPARE(parsed.functions[index], vendor.functions[index]);
    QCOMPARE(parsed.names.size(), vendor.names.size());
    QVERIFY(!createFile(directory.filePath("OriginalVendor.dll"), origin).isEmpty());
    QVERIFY(!createFile(directory.filePath("OriginalData.dll"), own).isEmpty());
    const auto path = createFile(directory.filePath("OriginalShell.dll"), shell); QVERIFY(!path.isEmpty());
    QLibrary originLibrary(directory.filePath("OriginalVendor.dll")), dataLibrary(directory.filePath("OriginalData.dll"));
    QVERIFY2(originLibrary.load(), qPrintable(originLibrary.errorString()));
    QVERIFY2(dataLibrary.load(), qPrintable(dataLibrary.errorString()));
    const auto module = LoadLibraryW(reinterpret_cast<LPCWSTR>(path.utf16()));
    QVERIFY2(module != nullptr, qPrintable(QString::number(GetLastError())));
    using Query = DWORD (WINAPI*)();
    const auto overridden = reinterpret_cast<Query>(GetProcAddress(module, "xlOpenDriver"));
    const auto untouched = reinterpret_cast<Query>(GetProcAddress(module, "VendorOnly"));
    const auto opaque = reinterpret_cast<Query>(GetProcAddress(module, "?Opaque@@YAHXZ"));
    const auto ordinal = reinterpret_cast<Query>(GetProcAddress(module, MAKEINTRESOURCEA(22)));
    const bool exportsResolved = overridden && untouched && opaque && ordinal;
    DWORD thread = 0, process = 0, decorated = 0, unnamed = 0;
    if (exportsResolved) { thread = overridden(); process = untouched(); decorated = opaque(); unnamed = ordinal(); }
    FreeLibrary(module);
    QVERIFY(exportsResolved); QCOMPARE(thread, GetCurrentThreadId()); QCOMPARE(process, GetCurrentProcessId());
    QCOMPARE(decorated, GetCurrentProcessId()); QCOMPARE(unnamed, GetCurrentProcessId());
    for (int length : {0, 63, 128, 512}) QVERIFY(!readPeExports(origin.left(length), &parsed, &error));
    QVERIFY(makeCanoeForwarder(origin, own, machine == IMAGE_FILE_MACHINE_I386 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386,
        "Shell.dll", "OriginalVendor", "OriginalData", &error).isEmpty());
    data.ordinalBase = 65535;
    QVERIFY(makePeForwarder(data, "Overflow.dll", {{0,"KERNEL32.GetCurrentThreadId"},{1,"KERNEL32.GetCurrentThreadId"}}, &error).isEmpty());
}

void ManagerModelTest::canoeFirstLaunchCreatesOwnedCopy() {
    using namespace Ucal::Manager;
    QTemporaryDir directory; QVERIFY(directory.isValid());
    struct Restore { QByteArray local = qgetenv("LOCALAPPDATA"); ~Restore() { qputenv("LOCALAPPDATA", local); } } restore;
    const auto local = directory.filePath("local"); QVERIFY(QDir().mkpath(local)); qputenv("LOCALAPPDATA", local.toUtf8());
    const auto vendor = directory.filePath("installed");
    const auto save = [](const QString& path, const QByteArray& bytes) {
        return QDir().mkpath(QFileInfo(path).absolutePath()) && !createFile(path, bytes).isEmpty();
    };
    for (const auto& relative : {"vcustom.ini", "Exec64/CANoe64.exe", "Exec64/CANoe01.dll",
            "Exec64/Vector.CANalyzer.CommonDialogs.dll", "Exec32/RuntimeKernel.exe", "Exec32/RTMain.dll", "Exec32/RTFrameworks.dll",
            "Exec64/managed/OriginalFixture.dll"}) QVERIFY(save(QDir(vendor).filePath(relative), "original-installed-fixture"));
    const auto product = directory.filePath("product");
    CanoeRuntimeSources sources;
    for (const bool x64 : {false, true}) {
        PeExportTable table; table.machine = x64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
        table.ordinalBase = 1; table.functions = {true, true}; table.names = {{"xlOpenDriver", 0}, {"xlGetDriverConfigEx", 1}};
        QString error;
        const auto image = makePeForwarder(table, "OriginalFixture.dll", {{0,"KERNEL32.GetCurrentThreadId"},{1,"KERNEL32.GetCurrentThreadId"}}, &error);
        QVERIFY2(!image.isEmpty(), qPrintable(error));
        const auto arch = x64 ? QStringLiteral("x64") : QStringLiteral("x86");
        const auto runtime = directory.filePath("drivers/" + arch + ".dll"); QVERIFY(save(runtime, image));
        if (x64) sources.x64 = runtime; else sources.x86 = runtime;
        const auto package = QDir(product).filePath("frontends/canoe/" + arch);
        QVERIFY(save(QDir(package).filePath(x64 ? "UcalCanoeProvider64.dll" : "UcalCanoeProvider.dll"), image));
        QVERIFY(save(QDir(package).filePath(x64 ? "UcalVcndrvmsDataIntercept64.dll" : "UcalVcndrvmsDataIntercept.dll"), image));
        QVERIFY(save(QDir(product).filePath("frontends/vxlapi/" + arch + '/' + (x64 ? "UcalXlDiscovery64.dll" : "UcalXlDiscovery.dll")), image));
    }
    LaunchPlan plan; plan.softwareExecutable = QDir(vendor).filePath("Exec64/CANoe64.exe");
    plan.softwareInstallationId = "canoe:first-launch-fixture"; plan.integrationAdapterId = "vxl-process-route";
    plan.softwareVersion = "12.0.75.0";
    QString error;
    QVERIFY2(inspectCanoeLaunchCopyInputs(plan, product, sources, &error), qPrintable(error));
    QVERIFY(!QFileInfo(QDir(local).filePath("UCAL")).exists());
    const auto target = QDir(local).filePath("UCAL/canoe-shadow/canoe-first-launch-fixture");
    QCOMPARE(canoeLaunchCopyPath(plan.softwareInstallationId, "11.0.42.0"), target);
    QVERIFY(!provisionCanoeLaunchCopy(plan, product, sources, &error, [] { return false; }));
    QVERIFY(!QFileInfo(target).exists());
    QVERIFY(save(QDir(target).filePath("user-settings.ini"), "retain-incomplete-copy"));
    QVERIFY2(provisionCanoeLaunchCopy(plan, product, sources, &error), qPrintable(error));
    QVERIFY(completeCanoeLaunchCopy(target));
    QVERIFY(QFileInfo(QDir(target).filePath("Exec64/managed/OriginalFixture.dll")).isFile());
    const auto previous = QDir(QFileInfo(target).absolutePath()).entryList({"canoe-first-launch-fixture.previous-*"}, QDir::Dirs);
    QCOMPARE(previous.size(), 1);
    QVERIFY(QFileInfo(QDir(QFileInfo(target).absolutePath()).filePath(previous[0] + "/user-settings.ini")).isFile());
    QVERIFY(save(QDir(target).filePath("vcustom.ini"), "saved-user-settings"));
    QVERIFY2(provisionCanoeLaunchCopy(plan, product, sources, &error), qPrintable(error));
    QFile settings(QDir(target).filePath("vcustom.ini")); QVERIFY(settings.open(QIODevice::ReadOnly)); QCOMPARE(settings.readAll(), QByteArray("saved-user-settings"));
    QFile original(QDir(vendor).filePath("vcustom.ini")); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("original-installed-fixture"));
    for (const auto& exec : {QStringLiteral("Exec32"), QStringLiteral("Exec64")}) {
        QFile shell(QDir(target).filePath(exec + '/' + (exec == "Exec32" ? "vcndrvms.dll" : "vcndrvms64.dll")));
        QVERIFY(shell.open(QIODevice::ReadOnly)); PeExportTable table; QVERIFY(readPeExports(shell.readAll(), &table, &error)); QCOMPARE(table.names.size(), 2);
    }
    // Preserve a broken copy and rebuild with the same saved installation/profile.
    settings.close();
    const auto savedPlan = plan.toJson();
    const auto retained = target + QStringLiteral(".retained");
    QVERIFY(QDir().rename(target, retained));
    QVERIFY2(inspectCanoeLaunchCopyInputs(plan, product, sources, &error), qPrintable(error));
    QVERIFY(!QFileInfo(target).exists());
    QVERIFY2(provisionCanoeLaunchCopy(plan, product, sources, &error), qPrintable(error));
    QVERIFY(completeCanoeLaunchCopy(target));
    QCOMPARE(plan.toJson(), savedPlan);
    QFile retainedSettings(QDir(retained).filePath("vcustom.ini"));
    QVERIFY(retainedSettings.open(QIODevice::ReadOnly));
    QCOMPARE(retainedSettings.readAll(), QByteArray("saved-user-settings"));
    const auto deepLocal = QDir(local).filePath(QString(94, QLatin1Char('x')));
    QVERIFY(QDir().mkpath(deepLocal));
    qputenv("LOCALAPPDATA", deepLocal.toUtf8());
    const auto deepLegacy = QDir(deepLocal).filePath("UCAL/canoe-shadow/canoe-first-launch-fixture");
    LaunchPlan legacyPlan = plan;
    // Recreate the layout written before either version used compact paths.
    legacyPlan.softwareVersion.clear();
    QCOMPARE(canoeLaunchCopyPath(plan.softwareInstallationId, legacyPlan.softwareVersion), deepLegacy);
    QVERIFY2(provisionCanoeLaunchCopy(legacyPlan, product, sources, &error), qPrintable(error));
    const auto legacySettingsPath = QDir(deepLegacy).filePath("vcustom.ini");
    QVERIFY(save(legacySettingsPath, "saved-legacy-settings"));
    const auto compact = canoeLaunchCopyPath(plan.softwareInstallationId, plan.softwareVersion);
    QCOMPARE(canoeLaunchCopyPath(plan.softwareInstallationId, "11.0.42.0"), compact);
    QVERIFY(compact.startsWith(QDir(deepLocal).filePath("UCAL/c/")));
    QVERIFY(compact.size() < deepLegacy.size());
    QVERIFY(canoeLaunchCopyPath("canoe:another-installation", plan.softwareVersion) != compact);
    LaunchPlan canoe11Plan = plan;
    canoe11Plan.softwareVersion = "11.0.42.0";
    QVERIFY2(provisionCanoeLaunchCopy(canoe11Plan, product, sources, &error), qPrintable(error));
    QVERIFY(completeCanoeLaunchCopy(compact));
    const auto compactSettingsPath = QDir(compact).filePath("vcustom.ini");
    QFile compactSettings(compactSettingsPath);
    QVERIFY(compactSettings.open(QIODevice::ReadOnly));
    QCOMPARE(compactSettings.readAll(), QByteArray("saved-legacy-settings"));
    compactSettings.close();
    QVERIFY(save(compactSettingsPath, "updated-compact-settings"));
    QVERIFY2(provisionCanoeLaunchCopy(plan, product, sources, &error), qPrintable(error));
    QVERIFY(compactSettings.open(QIODevice::ReadOnly));
    QCOMPARE(compactSettings.readAll(), QByteArray("updated-compact-settings"));
    QFile legacySettings(legacySettingsPath);
    QVERIFY(legacySettings.open(QIODevice::ReadOnly));
    QCOMPARE(legacySettings.readAll(), QByteArray("saved-legacy-settings"));
    QCOMPARE(plan.toJson(), savedPlan);
    qputenv("LOCALAPPDATA", local.toUtf8());
    plan.softwareInstallationId = "canoe:../outside";
    QVERIFY(!provisionCanoeLaunchCopy(plan, product, sources, &error));
    plan.softwareInstallationId = "canoe:missing-package";
    QVERIFY(!provisionCanoeLaunchCopy(plan, directory.filePath("empty-package"), sources, &error));
    QVERIFY(!QFileInfo(QDir(local).filePath("UCAL/canoe-shadow/canoe-missing-package")).exists());
    qputenv("LOCALAPPDATA", vendor.toUtf8());
    QVERIFY(!provisionCanoeLaunchCopy(plan, product, sources, &error));
    QVERIFY(!QFileInfo(QDir(vendor).filePath("UCAL")).exists());
}

void ManagerModelTest::initTestCase() {
    QVERIFY(QStandardPaths::isTestModeEnabled());
    QVERIFY2(QGuiApplication::platformName() == QStringLiteral("windows"),
        "The Qt 5.14.2 Windows widget suite requires the windows QPA plugin; offscreen does not implement its native dialog contract.");
    QVERIFY2(QFileInfo(qEnvironmentVariable("UCAL_FRONTEND_FIXTURE_DLL")).isFile(),
        "Run manager/test/Test-UcalManager.ps1: it builds the original test fixtures and configures an isolated test environment.");
    QVERIFY(QFileInfo(qEnvironmentVariable("UCAL_MANAGER_ACCEPTANCE_EXE")).isFile());
    QVERIFY(QFileInfo(QDir(qEnvironmentVariable("UCAL_FRONTEND_DIRECTORY"))
        .filePath(QStringLiteral("canoe/frontend.json"))).isFile());
    QVERIFY(QDir(qEnvironmentVariable("UCAL_BACKEND_DIRECTORY"))
        .entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}


void ManagerModelTest::coldStartupDoesNotWaitForBroker() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto previous = qgetenv("UCAL_BROKER_INSTANCE");
    struct Restore { QByteArray value; ~Restore(){qputenv("UCAL_BROKER_INSTANCE", value);} } restore{previous};
    qputenv("UCAL_BROKER_INSTANCE", QUuid::createUuid().toByteArray(QUuid::WithoutBraces));
    QElapsedTimer elapsed;
    elapsed.start();
    {
        // Do not process events: the constructor must return before starting
        // asynchronous discovery, and destruction cancels its queued callback.
        Ucal::Manager::MainWindow window(PresentationMode::Production, UiLanguage::English,
            false, true, directory.filePath("profiles.json"));
        QVERIFY2(elapsed.elapsed() < 2000, "Window construction waited for a missing Broker");
    }
}

void ManagerModelTest::unavailableDeploymentActionsAreHidden() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto workspace = directory.filePath("profiles.json");
    QString error;
    QVERIFY2(ManagerModel::realisticDemo().saveToFile(workspace, &error), qPrintable(error));
    Ucal::Manager::MainWindow window(PresentationMode::Production, UiLanguage::English, false, true, workspace);
    const auto deployment = Ucal::Manager::uiText(UiLanguage::English, QStringLiteral("apps.menu.deploy"));
    // Ensure real application-card menus were built before testing absence.
    const auto check = Ucal::Manager::uiText(UiLanguage::English, QStringLiteral("apps.menu.check"));
    bool hasApplicationMenu = false;
    for (const auto* action : window.findChildren<QAction*>()) {
        hasApplicationMenu |= action->text() == check;
        QVERIFY(action->text() != deployment);
    }
    QVERIFY(hasApplicationMenu);
}

void ManagerModelTest::compatibilityChecksRequiredLaunchAssets() {
    // A supported application-local profile does not qualify the separate Provider.
    const auto compatibility = [](const QString& version, const QString& architecture,
                                  const QString& adapter) {
        return Ucal::Manager::invokeFrontend("canoe", "check_compatibility",
            {{"softwareVersion", version}, {"softwareArchitecture", architecture},
             {"integrationAdapterId", adapter}, {"inspectLaunchAssets", false}});
    };
    const auto canoe11 = compatibility("11.0.42.0", "x64", "vxl-process-route");
    QVERIFY2(canoe11.ok(), qPrintable(canoe11.error));
    QVERIFY(canoe11.value.value("blockers").toArray().isEmpty());
    for (const auto& context : QVector<QStringList>{
             {"11.0.42.0", "x64", "canoe-provider"},
             {"11.0.42.0", "x86", "vxl-process-route"},
             {"13.0.0.0", "x64", "vxl-process-route"},
             {"unknown", "x64", "vxl-process-route"}}) {
        const auto rejected = compatibility(context[0], context[1], context[2]);
        QVERIFY2(rejected.ok(), qPrintable(rejected.error));
        QVERIFY(!rejected.value.value("blockers").toArray().isEmpty());
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto executable = createFile(directory.filePath("CANoe64.exe"));
    QJsonObject plan{{"ready", true}, {"applicationId", "readiness-test"},
        {"softwareInstallationId", "readiness-test"}, {"integrationAdapterId", "vxl-process-route"},
        {"software", QJsonObject{{"family", "canoe"}, {"version", "12.0.75.0"},
            {"architecture", "x64"}, {"executablePath", executable}}},
        {"launcher", QJsonObject{{"repositoryRoot", directory.path()}}},
        {"endpoints", QJsonArray{QJsonObject{{"endpointId", "11111111-1111-4111-8111-111111111151"}}}}};
    QJsonObject context{{"softwareArchitecture", "x64"}, {"softwareVersion", "12.0.75.0"},
        {"integrationAdapterId", "vxl-process-route"}, {"language", int(UiLanguage::English)},
        {"inspectLaunchAssets", true}, {"applicationDirectory", directory.path()}, {"plan", plan}};
    const auto checked = Ucal::Manager::invokeFrontend("canoe", "check_compatibility", context);
    QVERIFY2(checked.ok(), qPrintable(checked.error));
    QVERIFY(!checked.value.value("blockers").toArray().isEmpty());
    QVERIFY(!QFileInfo(directory.filePath("Exec32")).exists());
    const auto prepared = Ucal::Manager::invokeFrontend("canoe", "prepare_launch",
        {{"plan", plan}, {"language", int(UiLanguage::English)},
         {"applicationDirectory", directory.path()}, {"temporaryDirectory", directory.path()}});
    QVERIFY(!prepared.ok());
    QCOMPARE(prepared.error, checked.value.value("blockers").toArray().first().toString() +
        QStringLiteral(" (code %1)").arg(prepared.code));

    auto software = plan.value("software").toObject();
    software.insert("family", "pcanview");
    software.insert("version", "6.0.0.1040");
    plan.insert("software", software);
    plan.insert("integrationAdapterId", "pcanview5-6-structural-adapter");
    context.insert("softwareVersion", "6.0.0.1040");
    context.insert("integrationAdapterId", "pcanview5-6-structural-adapter");
    context.insert("channelCount", 1);
    context.insert("plan", plan);
    const auto pcanChecked = Ucal::Manager::invokeFrontend("pcanview", "check_compatibility", context);
    QVERIFY2(pcanChecked.ok(), qPrintable(pcanChecked.error));
    QVERIFY(!pcanChecked.value.value("blockers").toArray().isEmpty());
    const auto pcanPrepared = Ucal::Manager::invokeFrontend("pcanview", "prepare_launch",
        {{"plan", plan}, {"language", int(UiLanguage::English)},
         {"applicationDirectory", directory.path()}, {"temporaryDirectory", directory.path()}});
    QVERIFY(!pcanPrepared.ok());
    QCOMPARE(pcanPrepared.error, pcanChecked.value.value("blockers").toArray().first().toString() +
        QStringLiteral(" (code %1)").arg(pcanPrepared.code));
}

void ManagerModelTest::canoeDriverRegistrationPreflight() {
    RegistryFixture fixture;
    const auto root = fixture.root + QStringLiteral("\\Drivers");
    for (const auto language : {UiLanguage::English, UiLanguage::SimplifiedChinese,
                               UiLanguage::German, UiLanguage::Japanese}) {
        const auto missing = Ucal::Manager::canoeDriverRegistrationError(language, root);
        QVERIFY(missing.contains(root));
        QVERIFY(missing.contains(root + QStringLiteral("\\VN1610")));
        QVERIFY(missing.contains(QStringLiteral("Vector Driver Setup")));
        QVERIFY(!missing.contains(QStringLiteral("%1")));
    }
    QSettings owned(fixture.root, QSettings::Registry64Format);
    QVERIFY(!owned.childGroups().contains(QStringLiteral("Drivers")));
    owned.setValue(QStringLiteral("Drivers/fixture"), true);
    owned.sync();
    QVERIFY(!Ucal::Manager::canoeDriverRegistrationError(UiLanguage::English, root).isEmpty());
    // A legitimate Virtual-only installation must not require a VN1610.
    owned.setValue(QStringLiteral("Drivers/Virtual/DriverName"), QStringLiteral("vcanv"));
    owned.sync();
    QVERIFY(Ucal::Manager::canoeDriverRegistrationError(UiLanguage::English, root).isEmpty());
    const auto before = owned.allKeys();
    QVERIFY(Ucal::Manager::canoeDriverRegistrationError(UiLanguage::SimplifiedChinese, root).isEmpty());
    QCOMPARE(owned.allKeys(), before);
    owned.remove(QStringLiteral("Drivers/Virtual"));
    owned.setValue(QStringLiteral("Drivers/VN1610/DriverName"), QStringLiteral("VN1610"));
    owned.sync();
    QVERIFY(Ucal::Manager::canoeDriverRegistrationError(UiLanguage::English, root).isEmpty());
    const auto unreadable = Ucal::Manager::canoeDriverRegistrationError(
        UiLanguage::SimplifiedChinese, QStringLiteral("invalid-registry-root"));
    QVERIFY(unreadable.contains(QStringLiteral("无法读取")));
    QVERIFY(unreadable.contains(QStringLiteral("invalid-registry-root")));
}

void ManagerModelTest::duplicateEndpointMappingsAreRejectedAtomically() {
    auto model = ManagerModel::demo();
    const auto id = model.applications().first().id;
    const auto first = model.endpoints().at(0).id;
    const auto second = model.endpoints().at(1).id;
    QVERIFY(model.replaceApplicationChannels(id, {first, second}));
    const auto before = model.toJson();
    QVERIFY(!model.replaceApplicationChannels(id, {first, first}));
    QCOMPARE(model.toJson(), before);
    QVERIFY(!model.setApplicationMapping(id, 2, first));
    QCOMPARE(model.toJson(), before);
    model.setMapping(2, first);
    QCOMPARE(model.toJson(), before);
    QVERIFY(model.setApplicationMapping(id, 1, first));
    QString other;
    QVERIFY(model.addApplication(model.applicationById(id)->softwareInstallationId,
        QStringLiteral("Independent application"), &other));
    QVERIFY(model.replaceApplicationChannels(other, {first}));
    QVERIFY(model.replaceApplicationChannels(id, {QString(), QString()}));
    const QVector<Ucal::Manager::ChannelMapping> hidden{{2, first}, {1, first}};
    QVERIFY(Ucal::Manager::duplicateEndpointChannels(hidden, 1).isEmpty());
    // Qt 5.14's inline QVector equality uses stdext checked iterators removed
    // from newer MSVC libraries. Compare the complete contents explicitly.
    auto compareSlots = [](const QVector<int>& actual, std::initializer_list<int> expected) {
        QCOMPARE(actual.size(), int(expected.size()));
        int index = 0;
        for (const int slot : expected) QCOMPARE(actual.at(index++), slot);
    };
    compareSlots(Ucal::Manager::duplicateEndpointChannels(hidden, (std::numeric_limits<int>::max)()), {1, 2});
    const QVector<Ucal::Manager::ChannelMapping> groups{
        {6, second}, {3, first}, {1, first}, {5, QStringLiteral("independent")},
        {4, second}, {2, first}, {0, first}, {99, first}, {7, QString()}, {8, QString()}};
    compareSlots(Ucal::Manager::duplicateEndpointChannels(groups, 8), {1, 2, 3, 4, 6});
    const auto three = Ucal::Manager::duplicateEndpointChannels(groups, 3);
    compareSlots(three, {1, 2, 3});
    QCOMPARE(Ucal::Manager::duplicateEndpointMessage(UiLanguage::SimplifiedChinese, three),
        QStringLiteral("以下应用通道存在重复绑定：CAN 1、CAN 2、CAN 3。请为每个应用通道选择不同的硬件端点。"));
    QVERIFY(Ucal::Manager::duplicateEndpointMessage(UiLanguage::English, {}).isEmpty());
}

void ManagerModelTest::duplicateLegacyEditorCanBeCorrected() {
    QTemporaryDir directory;
    const auto workspace = directory.filePath(QStringLiteral("legacy-duplicate.json"));
    auto model = ManagerModel::demo();
    QVERIFY(model.setActiveProject(QStringLiteral("default")));
    const auto id = model.applications().first().id;
    const auto first = model.endpoints().at(0).id;
    const auto second = model.endpoints().at(1).id;
    QVERIFY(model.replaceApplicationChannels(id, {first, second, QString(), QString(), QString()}));
    QVERIFY(model.replaceApplicationChannels(model.applications().at(1).id, {second}));
    // Imported legacy data must remain editable; loading must not silently
    // discard the application or repair its user-selected mappings.
    model.mutableApplicationById(id)->mappings[1].endpointId = first;
    model.mutableApplicationById(id)->mappings[2].endpointId = first;
    model.mutableApplicationById(id)->mappings[3].endpointId = second;
    model.mutableApplicationById(id)->mappings[4].endpointId = second;
    QVERIFY(model.saveToFile(workspace));
    QFile file(workspace); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto original = file.readAll(); file.close();
    Ucal::Manager::LaunchEnvironment environment;
    const auto plan = LaunchCoordinator::prepare(model, id, true, false, environment);
    QVERIFY(!plan.ready);
    const auto completeWarning = Ucal::Manager::duplicateEndpointMessage(UiLanguage::English, {1, 2, 3, 4, 5});
    QCOMPARE(plan.blockers.filter(QStringLiteral("Duplicate hardware bindings")), QStringList{completeWarning});
    Ucal::Manager::MainWindow window(PresentationMode::Explanatory, UiLanguage::English, false, true, workspace);
    window.show();
    bool cancelled = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog); QTimer::singleShot(5000, dialog, &QDialog::reject);
        auto* error = dialog->findChild<QLabel*>(QStringLiteral("mappingValidationMessage"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(error && buttons && error->isVisible());
        QCOMPARE(error->text(), completeWarning);
        QVERIFY(!buttons->button(QDialogButtonBox::Save)->isEnabled());
        cancelled = true; buttons->button(QDialogButtonBox::Cancel)->click();
    });
    window.showApplicationEditorForReview(1);
    QVERIFY(cancelled); QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original); file.close();
    bool corrected = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog); QTimer::singleShot(5000, dialog, &QDialog::reject);
        auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("applicationChannelTable"));
        auto* save = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        auto* selected = dialog->findChild<QRadioButton*>(QStringLiteral("assignmentSelected"));
        auto* full = dialog->findChild<QRadioButton*>(QStringLiteral("assignmentFullAccess"));
        QVERIFY(table && selected && full);
        full->setChecked(true); QVERIFY(save->isEnabled());
        selected->setChecked(true); QVERIFY(!save->isEnabled());
        auto choice = [&](int row) { return qobject_cast<QComboBox*>(table->cellWidget(row, 1)); };
        auto* secondChoice = choice(1);
        // Persisted references are offline until a backend scan supplies a
        // current catalog. Clearing a duplicate must still be possible.
        secondChoice->setCurrentIndex(0);
        QVERIFY(!save->isEnabled());
        QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("mappingValidationMessage"))->text(),
            Ucal::Manager::duplicateEndpointMessage(UiLanguage::English, {1, 3, 4, 5}));
        choice(2)->setCurrentIndex(0);
        QVERIFY(!save->isEnabled());
        QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("mappingValidationMessage"))->text(),
            Ucal::Manager::duplicateEndpointMessage(UiLanguage::English, {4, 5}));
        choice(4)->setCurrentIndex(0);
        QVERIFY(save->isEnabled());
        QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("mappingValidationMessage"))->isHidden());
        corrected = true; save->click();
    });
    window.showApplicationEditorForReview(1); QVERIFY(corrected);
    ManagerModel saved; QVERIFY(ManagerModel::loadFromFile(workspace, &saved));
    const auto* app = saved.applicationById(id); QVERIFY(app);
    QCOMPARE(app->mappings.at(0).endpointId, first);
    QVERIFY(app->mappings.at(1).endpointId.isEmpty());
    QVERIFY(app->mappings.at(2).endpointId.isEmpty());
    QCOMPARE(app->mappings.at(3).endpointId, second);
    QVERIFY(app->mappings.at(4).endpointId.isEmpty());
}

void ManagerModelTest::endpointChoicesPreventDuplicateBindings() {
    Ucal::Manager::MainWindow window(PresentationMode::Realistic, UiLanguage::English, false, false);
    window.show();
    bool checked = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog); QTimer::singleShot(5000, dialog, &QDialog::reject);
        dialog->findChild<QRadioButton*>(QStringLiteral("assignmentSelected"))->setChecked(true);
        auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("applicationChannelTable"));
        QVERIFY(table);
        while (table->rowCount() < 3) {
            auto* add = qobject_cast<QPushButton*>(table->cellWidget(table->rowCount() - 1, 0));
            QVERIFY(add && add->isEnabled()); add->click();
            qobject_cast<QComboBox*>(table->cellWidget(table->rowCount() - 2, 1))->hidePopup();
        }
        auto choice = [&](int row) { return qobject_cast<QComboBox*>(table->cellWidget(row, 1)); };
        for (int row = 0; row < table->rowCount() - 1; ++row) choice(row)->setCurrentIndex(0);
        const auto first = choice(0)->itemData(1).toString();
        const auto second = choice(0)->itemData(2).toString();
        QVERIFY(!first.isEmpty() && first != second);
        const auto suffix = Ucal::Manager::uiText(UiLanguage::English,
            QStringLiteral("mapping.endpointAssignedSuffix"));
        auto text = [](QComboBox* combo, const QString& endpoint) {
            return combo->itemText(combo->findData(endpoint));
        };
        const auto firstLabel = text(choice(0), first);
        const auto secondLabel = text(choice(0), second);
        auto enabled = [](QComboBox* combo, const QString& endpoint) {
            auto* items = qobject_cast<QStandardItemModel*>(combo->model());
            return items->item(combo->findData(endpoint))->isEnabled();
        };
        choice(0)->setCurrentIndex(choice(0)->findData(first));
        choice(1)->setCurrentIndex(choice(1)->findData(second));
        QVERIFY(!enabled(choice(1), first)); QVERIFY(!enabled(choice(0), second));
        QVERIFY(choice(1)->itemData(choice(1)->findData(first), Qt::ForegroundRole).canConvert<QBrush>());
        QVERIFY(!choice(0)->itemData(choice(0)->findData(first), Qt::ForegroundRole).isValid());
        QCOMPARE(text(choice(1), first), firstLabel + suffix);
        QCOMPARE(text(choice(0), second), secondLabel + suffix);
        QCOMPARE(text(choice(0), first), firstLabel);
        // Repeated updates must not append another marker or change endpoint ID.
        choice(1)->setCurrentIndex(0);
        choice(1)->setCurrentIndex(choice(1)->findData(second));
        QCOMPARE(text(choice(1), first), firstLabel + suffix);
        QCOMPARE(text(choice(0), second), secondLabel + suffix);
        QVERIFY(choice(1)->itemData(choice(1)->findData(first), Qt::ToolTipRole).toString().isEmpty());
        auto* save = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        QVERIFY(save->isEnabled());
        // Exercise save validation even if a programmatic caller bypasses the
        // disabled dropdown item, as an imported duplicate would do.
        choice(1)->setCurrentIndex(choice(1)->findData(first));
        QVERIFY(!save->isEnabled());
        QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("mappingValidationMessage"))->isVisible());
        choice(0)->setCurrentIndex(0);
        QVERIFY(enabled(choice(1), first));
        QVERIFY(!choice(1)->itemData(choice(1)->findData(first), Qt::ForegroundRole).isValid());
        QCOMPARE(text(choice(1), first), firstLabel);
        choice(0)->setCurrentIndex(choice(0)->findData(second));
        QVERIFY(save->isEnabled());
        // Remove the row that owns the endpoint; the remaining row must regain
        // its original available label and keep its own selection unchanged.
        QTimer::singleShot(0, [] {
            auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(confirmation); confirmation->button(QMessageBox::Yes)->click();
        });
        qobject_cast<QPushButton*>(table->cellWidget(0, 3))->click();
        QCOMPARE(text(choice(0), second), secondLabel);
        QVERIFY(enabled(choice(0), second));
        QCOMPARE(choice(0)->currentData().toString(), first);
        checked = true; dialog->reject();
    });
    window.showApplicationEditorForReview(1); QVERIFY(checked);
}

void ManagerModelTest::stagedEditorKeepsCurrentRuntimeState() {
    auto current = ManagerModel::demo();
    const auto id = current.applications().first().id;
    const auto endpoint = current.endpoints().first().id;
    QVERIFY(current.replaceApplicationChannels(id, {endpoint}));
    auto staged = current;
    staged.mutableApplicationById(id)->name = QStringLiteral("Edited while device changed");
    current.reconcileInventory({});
    current.reconcileSoftwareInstallations({});
    staged.refreshRuntimeStateFrom(current);
    QCOMPARE(staged.applicationById(id)->name, QStringLiteral("Edited while device changed"));
    QCOMPARE(staged.applicationById(id)->mappings.first().endpointId, endpoint);
    QCOMPARE(staged.endpointById(endpoint)->state, EndpointState::Offline);
    QCOMPARE(staged.endpointById(endpoint)->historicalBinding, current.endpointById(endpoint)->historicalBinding);
    QVERIFY(!staged.softwareById(staged.applicationById(id)->softwareInstallationId)->available);
}

void ManagerModelTest::hardwareNotificationsCoalesceAndSettle() {
    Ucal::Manager::HardwareChangeMonitor monitor(nullptr, 30, 200);
    // This case supplies synthetic hints; native Windows delivery has its own case.
    QCoreApplication::instance()->removeNativeEventFilter(&monitor);
    QSignalSpy observed(&monitor, &Ucal::Manager::HardwareChangeMonitor::changeObserved);
    QSignalSpy refresh(&monitor, &Ucal::Manager::HardwareChangeMonitor::refreshRequested);
    bool firstRefreshObserved = false;
    connect(&monitor, &Ucal::Manager::HardwareChangeMonitor::refreshRequested, &monitor, [&] {
        if (!firstRefreshObserved) {
            firstRefreshObserved = true;
            // Both timers may become due before the next QTRY polling turn.
            // Check the intermediate count at the first signal itself.
            QCOMPARE(refresh.count(), 1);
        }
    });
    for (int i = 0; i < 20; ++i) monitor.observeChange();
    QCOMPARE(observed.count(), 20); QCOMPARE(monitor.generation(), quint64(20));
    QCOMPARE(refresh.count(), 0); QVERIFY(monitor.isDebouncing());
    QTRY_VERIFY_WITH_TIMEOUT(firstRefreshObserved, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(refresh.count(), 2, 1500);
    QTest::qWait(100); QCOMPARE(refresh.count(), 2);
    int afterDestruction = 0;
    auto* temporary = new Ucal::Manager::HardwareChangeMonitor(nullptr, 30, 100);
    connect(temporary, &Ucal::Manager::HardwareChangeMonitor::refreshRequested, this, [&] { ++afterDestruction; });
    temporary->observeChange(); delete temporary;
    QTest::qWait(160); QCOMPARE(afterDestruction, 0);
}

void ManagerModelTest::windowsDeviceChangeReachesQtMonitor() {
#ifdef Q_OS_WIN
    Ucal::Manager::HardwareChangeMonitor monitor(nullptr, 500, 2000);
    QSignalSpy observed(&monitor, &Ucal::Manager::HardwareChangeMonitor::changeObserved);
    MSG notification{}; notification.message = WM_DEVICECHANGE;
    notification.wParam = DBT_DEVICEQUERYREMOVE;
    long result = 123;
    QVERIFY(!monitor.nativeEventFilter("windows_generic_MSG", &notification, &result));
    QCOMPARE(observed.count(), 0); QCOMPARE(result, 123L);
    notification.wParam = DBT_DEVNODES_CHANGED;
    QVERIFY(!monitor.nativeEventFilter("not-a-windows-message", &notification, &result));
    QCOMPARE(observed.count(), 0);
    QWidget window; window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto handle = reinterpret_cast<HWND>(window.winId());
    DWORD_PTR delivered{};
    QVERIFY(SendMessageTimeoutW(handle, WM_DEVICECHANGE, DBT_DEVNODES_CHANGED, 0,
                               SMTO_ABORTIFHUNG, 1000, &delivered));
    QCOMPARE(observed.count(), 1);
    // Arrival/removal notifications carry a broadcast record. A null record is
    // invalid input to Windows message marshalling and may never reach Qt.
    DEV_BROADCAST_DEVICEINTERFACE_W device{};
    device.dbcc_size = sizeof(device);
    device.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    QVERIFY(SendMessageTimeoutW(handle, WM_DEVICECHANGE, DBT_DEVICEREMOVECOMPLETE,
                               reinterpret_cast<LPARAM>(&device), SMTO_ABORTIFHUNG,
                               1000, &delivered));
    QCOMPARE(observed.count(), 2);
    QCOMPARE(result, 123L);
#else
    QSKIP("Windows device notification acceptance");
#endif
}

void ManagerModelTest::hardwareOnlyDiscoverySkipsSoftware() {
    const auto original = qgetenv("UCAL_BROKER_INSTANCE");
    qputenv("UCAL_BROKER_INSTANCE", (QStringLiteral("hardware-only-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces)).toUtf8());
    const auto result = Ucal::Manager::discoverRuntimeEndpoints(false);
    qputenv("UCAL_BROKER_INSTANCE", original);
    QVERIFY(!result.softwareInventoryScanned);
    QVERIFY(result.softwareInstallations.isEmpty());
}

void ManagerModelTest::launchEnvironmentSurvivesWindowsDriveVariables() {
    QTemporaryDir dir;
    Ucal::Manager::ProductLaunchSpec spec;
    spec.program = QCoreApplication::applicationFilePath();
    spec.workingDirectory = dir.path();
    spec.arguments = QStringList{QStringLiteral("argument with spaces")};
    spec.environment.insert(QStringLiteral("=C:"), QStringLiteral("C:/work"));
    spec.environment.insert(QStringLiteral("UCAL_TEST"), QStringLiteral("retained"));
    auto restored = Ucal::Manager::productLaunchSpecFromJson(Ucal::Manager::productLaunchSpecToJson(spec));
    QVERIFY2(restored.valid(), qPrintable(restored.error));
    QCOMPARE(restored.environment.value(QStringLiteral("UCAL_TEST")), QStringLiteral("retained"));
    QCOMPARE(restored.arguments, spec.arguments);
    auto invalid = Ucal::Manager::productLaunchSpecToJson(spec);
    invalid.insert("environment", QJsonObject{{"INVALID=KEY", "bad"}});
    QVERIFY(!Ucal::Manager::productLaunchSpecFromJson(invalid).valid());
}
void ManagerModelTest::includeAllCustomProjectCanBeDeleted() {
    auto model = ManagerModel::demo();
    auto object = model.toJson();
    auto projects = object.value("projects").toArray();
    projects.append(QJsonObject{{"id", "include-all"}, {"name", "All applications"},
                               {"includeAllApplications", true}, {"applicationIds", QJsonArray{}}});
    object.insert("projects", projects);
    QVERIFY(ManagerModel::fromJson(object, &model));
    const auto count = model.applications().size();
    QVERIFY(model.removeProject(QStringLiteral("include-all")));
    QCOMPARE(model.applications().size(), count);
    QVERIFY(!model.removeProject(QStringLiteral("default")));
}
void ManagerModelTest::unsupportedSoftwareCannotCreateApplication() {
    QTemporaryDir dir;
    const auto exe = dir.filePath(QStringLiteral("unknown.exe"));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), exe));
    auto model = ManagerModel::productionDefaults();
    const auto id = model.addManualSoftware(exe);
    QVERIFY(!id.isEmpty());
    QVERIFY(!model.addApplication(id, QStringLiteral("Unsupported")));
    QVERIFY(model.applications().isEmpty());
}
void ManagerModelTest::pluginScanRestoresForgottenStandardInstallation() {
    QTemporaryDir dir;
    const auto folder = dir.filePath(QStringLiteral("PEAK-System/PCAN-View 6"));
    QVERIFY(QDir().mkpath(folder));
    const auto exe = QDir(folder).filePath(QStringLiteral("PcanView.exe"));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), exe));
    const auto before = qgetenv("ProgramFiles");
    qputenv("ProgramFiles", dir.path().toUtf8());
    auto model = ManagerModel::productionDefaults();
    const auto id = model.addManualSoftware(exe);
    const bool removed = !id.isEmpty() && model.removeManualSoftware(id);
    QStringList errors;
    const auto scan = Ucal::Manager::discoverInstalledSoftware(&errors);
    if (before.isNull()) qunsetenv("ProgramFiles"); else qputenv("ProgramFiles", before);
    QVERIFY(removed);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(';')));
    model.reconcileSoftwareInstallations(scan);
    QVERIFY(model.softwareById(id));
    QVERIFY(model.softwareById(id)->available);
    // Portable EXEs outside standard directories remain discoverable after record removal.
    const auto portable = dir.filePath(QStringLiteral("PcanView.exe"));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), portable));
    const auto portableId = model.addManualSoftware(portable);
    QVERIFY(model.removeManualSoftware(portableId));
    model.reconcileSoftwareInstallations(Ucal::Manager::discoverInstalledSoftware());
    QVERIFY(model.softwareById(portableId));
    QVERIFY(model.softwareById(portableId)->available);
}
void ManagerModelTest::portablePathsHonorIsolation() {
    const auto directory = QFileInfo(Ucal::Manager::uiLanguagePreferencePath()).absolutePath();
    QCOMPARE(QFileInfo(ManagerModel::productionStoragePath()).absolutePath(), directory);
    const auto override = qgetenv("UCAL_MANAGER_SETTINGS_DIRECTORY");
    QCOMPARE(QDir::cleanPath(directory), QDir::cleanPath(QString::fromUtf8(override)));
    struct Restore { QByteArray value; ~Restore() { qputenv("UCAL_MANAGER_SETTINGS_DIRECTORY", value); } } restore{override};
    qunsetenv("UCAL_MANAGER_SETTINGS_DIRECTORY");
    QCOMPARE(QFileInfo(Ucal::Manager::uiLanguagePreferencePath()).absolutePath(),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data")));
}

void ManagerModelTest::softwareRescanRunsWithoutBroker() {
    QTemporaryDir directory;
    Ucal::Manager::MainWindow window(PresentationMode::Production, UiLanguage::English,
        false, false, directory.filePath(QStringLiteral("workspace.json")));
    window.selectPage(2);
    QPushButton* scan = nullptr;
    for (auto* button : window.findChildren<QPushButton*>())
        if (button->property("automationId").toString() == QStringLiteral("rescanSoftware")) scan = button;
    QVERIFY(scan);
    scan->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().startsWith(QStringLiteral("Scan complete")), 10000);
    QVERIFY(!window.statusBar()->currentMessage().contains(QStringLiteral("Broker")));
}

void ManagerModelTest::phaseOneChannelCounts() {
    ManagerModel model = ManagerModel::demo();
    const QVector<int> validCounts = {1, 4, 8, 16};
    for (const int count : validCounts) {
        model.setChannelCount(count);
        QCOMPARE(model.channelCount(), count);
        QCOMPARE(model.visibleMappings().size(), count);
    }

    model.setChannelCount(17);
    QCOMPARE(model.channelCount(), 16);
    model.setChannelCount(0);
    QCOMPARE(model.channelCount(), 1);
}

void ManagerModelTest::missingEndpointKeepsSlot() {
    ManagerModel model = ManagerModel::demo();
    const QString endpointId = model.endpointIdForChannel(2);
    QVERIFY(!endpointId.isEmpty());

    QVector<Endpoint> live;
    for (const Endpoint& endpoint : model.endpoints()) {
        if (endpoint.id != endpointId) live.push_back(endpoint);
    }
    model.reconcileInventory(live);

    QCOMPARE(model.endpointIdForChannel(2), endpointId);
    const Endpoint* missing = model.endpointById(endpointId);
    QVERIFY(missing != nullptr);
    QCOMPARE(missing->state, EndpointState::Offline);
}

void ManagerModelTest::reorderedInventoryDoesNotRemap() {
    ManagerModel model = ManagerModel::demo();
    const QString first = model.endpointIdForChannel(1);
    const QString second = model.endpointIdForChannel(2);
    const QString third = model.endpointIdForChannel(3);

    QVector<Endpoint> reversed = model.endpoints();
    std::reverse(reversed.begin(), reversed.end());
    model.reconcileInventory(reversed);

    QCOMPARE(model.endpointIdForChannel(1), first);
    QCOMPARE(model.endpointIdForChannel(2), second);
    QCOMPARE(model.endpointIdForChannel(3), third);
}

void ManagerModelTest::roundTripsJsonAtomically() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profiles.json"));

    ManagerModel original = ManagerModel::demo();
    original.setChannelCount(8);
    original.reconcileInventory({Endpoint{
        QStringLiteral("endpoint:raw-index"), QStringLiteral("adapter:raw-index"),
        QStringLiteral("PCAN-Basic"), QStringLiteral("PEAK"), QStringLiteral("PCAN-USB FD"),
        QStringLiteral("duplicate"), QStringLiteral("CAN"), 1, QString(), QStringLiteral("CAN"),
        QStringLiteral("test"), EndpointState::Online, 0x51u, true}});
    original.setMapping(8, QStringLiteral("endpoint:raw-index"));
    QString error;
    QVERIFY2(original.saveToFile(path, &error), qPrintable(error));

    ManagerModel restored;
    QVERIFY2(ManagerModel::loadFromFile(path, &restored, &error), qPrintable(error));
    QCOMPARE(restored.channelCount(), 8);
    QCOMPARE(restored.endpointIdForChannel(1), original.endpointIdForChannel(1));
    QCOMPARE(restored.endpointIdForChannel(2), original.endpointIdForChannel(2));
    QCOMPARE(restored.endpoints().size(), original.endpoints().size());
    QCOMPARE(restored.projects().size(), original.projects().size());
    QCOMPARE(restored.applications().size(), original.applications().size());
    for (const auto& software : restored.softwareInstallations()) QVERIFY(!software.available);
    const Endpoint* rawIndex = restored.endpointById(QStringLiteral("endpoint:raw-index"));
    QVERIFY(rawIndex != nullptr);
    QVERIFY(rawIndex->physicalIndexValid);
    QCOMPARE(rawIndex->physicalIndex, quint32(0x51u));

    original.setChannelCount(4);
    QVERIFY2(original.saveToFile(path, &error), qPrintable(error));
    QVERIFY(QFile::exists(path + QStringLiteral(".bak")));
}

void ManagerModelTest::retiredBusPresetIsIgnored() {
    QJsonObject legacyCurrent = ManagerModel::demo().toJson();
    QJsonArray applications = legacyCurrent.value(QStringLiteral("applications")).toArray();
    QVERIFY(!applications.isEmpty());
    QJsonObject application = applications.first().toObject();
    application.insert(QStringLiteral("busPreset"), QStringLiteral("CAN 500 kbit/s"));
    applications.replace(0, application);
    legacyCurrent.insert(QStringLiteral("applications"), applications);

    QJsonArray projects = legacyCurrent.value(QStringLiteral("projects")).toArray();
    QVERIFY(!projects.isEmpty());
    QJsonObject project = projects.first().toObject();
    project.insert(QStringLiteral("busPreset"), QStringLiteral("CAN 500 kbit/s · FD 2 Mbit/s"));
    projects.replace(0, project);
    legacyCurrent.insert(QStringLiteral("projects"), projects);

    ManagerModel restored;
    QString error;
    QVERIFY2(ManagerModel::fromJson(legacyCurrent, &restored, &error), qPrintable(error));
    const QJsonObject normalized = restored.toJson();
    for (const QJsonValue& value : normalized.value(QStringLiteral("applications")).toArray()) {
        QVERIFY(!value.toObject().contains(QStringLiteral("busPreset")));
    }
    for (const QJsonValue& value : normalized.value(QStringLiteral("projects")).toArray()) {
        QVERIFY(!value.toObject().contains(QStringLiteral("busPreset")));
    }
}

void ManagerModelTest::corruptPrimaryRecoversFromBackup() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profiles.json"));
    QString error;

    ManagerModel model = ManagerModel::demo();
    model.setChannelCount(8);
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    model.setChannelCount(4);
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    QVERIFY(QFile::exists(path + QStringLiteral(".bak")));
    QVERIFY(!createFile(path, QByteArray("{not-json")).isEmpty());

    ManagerModel recovered;
    QVERIFY2(ManagerModel::loadFromFile(path, &recovered, &error), qPrintable(error));
    QCOMPARE(recovered.channelCount(), 8);
    QFile corruptPrimary(path);
    QVERIFY(corruptPrimary.open(QIODevice::ReadOnly));
    QCOMPARE(corruptPrimary.readAll(), QByteArray("{not-json"));
}

void ManagerModelTest::unsupportedFutureSchemaDoesNotRollback() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profiles.json"));
    QString error;

    ManagerModel model = ManagerModel::demo();
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    model.setChannelCount(8);
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    QVERIFY(!createFile(path, QByteArray("{\"schemaVersion\":99}")).isEmpty());

    ManagerModel loaded;
    QVERIFY(!ManagerModel::loadFromFile(path, &loaded, &error));
    QVERIFY(!error.isEmpty());
}

void ManagerModelTest::legacySchemaMigrates() {
    QJsonObject legacy;
    legacy.insert(QStringLiteral("schemaVersion"), 1);
    legacy.insert(QStringLiteral("channelCount"), 8);
    legacy.insert(QStringLiteral("inventoryCache"), QJsonArray());
    legacy.insert(QStringLiteral("mappings"), QJsonArray());

    ManagerModel migrated;
    QString error;
    QVERIFY2(ManagerModel::fromJson(legacy, &migrated, &error), qPrintable(error));
    QCOMPARE(migrated.channelCount(), 8);
    QVERIFY(!migrated.applications().isEmpty());
    QVERIFY(!migrated.projects().isEmpty());
}

void ManagerModelTest::saveFailsClosedForDirectoryTarget() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ManagerModel model = ManagerModel::demo();
    QString error;
    QVERIFY(!model.saveToFile(directory.path(), &error));
    QVERIFY(!error.isEmpty());
}

void ManagerModelTest::saveFailsClosedWhenBackupCannotBeWritten() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profiles.json"));
    const QString backupPath = path + QStringLiteral(".bak");
    ManagerModel model = ManagerModel::demo();
    QString error;
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = original.readAll();
    original.close();
    QVERIFY(QDir().mkpath(backupPath));

    model.setChannelCount(8);
    QVERIFY(!model.saveToFile(path, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), originalBytes);
}

void ManagerModelTest::saveFailsClosedForReadOnlyAndLockedTargets() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profiles.json"));
    ManagerModel model = ManagerModel::demo();
    QString error;
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    QFile snapshot(path);
    QVERIFY(snapshot.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = snapshot.readAll();
    snapshot.close();

#ifdef Q_OS_WIN
    const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
    const DWORD originalAttributes = GetFileAttributesW(nativePath.c_str());
    QVERIFY(originalAttributes != INVALID_FILE_ATTRIBUTES);
    QVERIFY(SetFileAttributesW(nativePath.c_str(),
                               originalAttributes | FILE_ATTRIBUTE_READONLY));
    model.setChannelCount(8);
    QVERIFY(!model.saveToFile(path, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(SetFileAttributesW(nativePath.c_str(), originalAttributes));
    QVERIFY(snapshot.open(QIODevice::ReadOnly));
    QCOMPARE(snapshot.readAll(), originalBytes);
    snapshot.close();

    HANDLE exclusive = CreateFileW(nativePath.c_str(), GENERIC_READ, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(exclusive != INVALID_HANDLE_VALUE);
    error.clear();
    model.setChannelCount(4);
    QVERIFY(!model.saveToFile(path, &error));
    QVERIFY(!error.isEmpty());
    CloseHandle(exclusive);
    QVERIFY(snapshot.open(QIODevice::ReadOnly));
    QCOMPARE(snapshot.readAll(), originalBytes);
#else
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser));
    model.setChannelCount(8);
    QVERIFY(!model.saveToFile(path, &error));
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
#endif
}

void ManagerModelTest::backupContainsExactPreviousGeneration() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profiles.json"));
    ManagerModel model = ManagerModel::demo();
    QString error;
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    QFile first(path);
    QVERIFY(first.open(QIODevice::ReadOnly));
    const QByteArray firstGeneration = first.readAll();
    first.close();

    model.setChannelCount(16);
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    QFile backup(path + QStringLiteral(".bak"));
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QCOMPARE(backup.readAll(), firstGeneration);
    ManagerModel previous;
    QVERIFY2(ManagerModel::loadFromFile(path + QStringLiteral(".bak"), &previous, &error),
             qPrintable(error));
    QCOMPARE(previous.channelCount(), 4);
}

void ManagerModelTest::corruptPrimaryAndBackupReportBothFailures() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profiles.json"));
    ManagerModel model = ManagerModel::demo();
    QString error;
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    model.setChannelCount(8);
    QVERIFY2(model.saveToFile(path, &error), qPrintable(error));
    QVERIFY(!createFile(path, QByteArray("{bad-primary")).isEmpty());
    QVERIFY(!createFile(path + QStringLiteral(".bak"), QByteArray("{bad-backup")).isEmpty());

    ManagerModel loaded;
    QVERIFY(!ManagerModel::loadFromFile(path, &loaded, &error));
    QVERIFY(error.contains(QStringLiteral("backup:")));
}

void ManagerModelTest::pluginDescriptionsAndChannelBindings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray pe(256, '\0'); pe[0]='M';pe[1]='Z';pe[0x3c]=char(0x80);
    pe[0x80]='P';pe[0x81]='E';pe[0x84]=char(0x64);pe[0x85]=char(0x86);pe[0x97]=char(0x20);
    const auto dll=createFile(directory.filePath(QStringLiteral("vendor.dll")),pe);
    QVERIFY(!dll.isEmpty());
    QJsonObject object{{QStringLiteral("schema"),QStringLiteral("org.ucal.backend-package/2")},
        {QStringLiteral("id"),QStringLiteral("bus-a")},{QStringLiteral("name"),QStringLiteral("Vendor J2534")},
        {QStringLiteral("vendor"),QStringLiteral("Vendor")},{QStringLiteral("version"),QStringLiteral("1.0.0")},
        {QStringLiteral("mode"),QStringLiteral("j2534")},{QStringLiteral("library"),dll}};
    Ucal::Manager::BackendPackage package;QString error;
    QVERIFY2(Ucal::Manager::parseBackendPlugin(object,directory.filePath(QStringLiteral("backend.json")),package,error),qPrintable(error));
    QCOMPARE(package.architecture,QStringLiteral("x64"));
    package.configurationPath=directory.filePath(QStringLiteral("channels.ini"));
    QVERIFY(Ucal::Manager::saveBackendPluginChannels(package,{},&error));
    QVERIFY(Ucal::Manager::loadBackendPluginChannels(package,&error).isEmpty());
    QVERIFY(error.isEmpty());
    Ucal::Manager::BackendChannelBinding a;
    a.identity=QStringLiteral("channel-a");a.name=QStringLiteral("CAN 1");a.selector=QStringLiteral("@Opaque(\\Device\\Port:0)");
    a.adapterIdentity=QStringLiteral("adapter-a");
    Ucal::Manager::BackendChannelBinding b=a;b.identity=QStringLiteral("channel-b");b.name=QStringLiteral("CAN 3");b.selector=QStringLiteral("\\\\Device\\Port:2");b.physicalPort=2;
    QVERIFY2(Ucal::Manager::saveBackendPluginChannels(package,{a,b},&error),qPrintable(error));
    const auto loaded=Ucal::Manager::loadBackendPluginChannels(package,&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(loaded.size(),2);
    QCOMPARE(loaded[0].selector,a.selector);QCOMPARE(loaded[1].selector,b.selector);QCOMPARE(loaded[1].physicalPort,quint32(2));
    QCOMPARE(loaded[0].adapterIdentity,a.adapterIdentity);QCOMPARE(loaded[1].adapterIdentity,a.adapterIdentity);
    b.physicalPort=0;QVERIFY(!Ucal::Manager::saveBackendPluginChannels(package,{a,b},&error));b.physicalPort=2;
    QFile saved(package.configurationPath);QVERIFY(saved.open(QIODevice::ReadOnly));const auto original=saved.readAll();saved.close();
    b.selector=a.selector;
    QVERIFY(!Ucal::Manager::saveBackendPluginChannels(package,{a,b},&error));
    QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(saved.readAll(),original);saved.close();
    a.selector.clear();b.selector.clear();
    QVERIFY(!Ucal::Manager::saveBackendPluginChannels(package,{a,b},&error));
    QVERIFY(Ucal::Manager::saveBackendPluginChannels(package,{a},&error));
    auto bad=object;bad.insert(QStringLiteral("configuration"),42);
    Ucal::Manager::BackendPackage rejected;QVERIFY(!Ucal::Manager::parseBackendPlugin(bad,QString(),rejected,error));
    for (const auto& id : {QStringLiteral("a"), QStringLiteral("b")}) {
        const auto folder=directory.filePath(id); QVERIFY(QDir().mkpath(folder));
        auto item=object; item.insert("id",id);
        QVERIFY(!createFile(QDir(folder).filePath("backend.json"),QJsonDocument(item).toJson()).isEmpty());
    }
    const auto scan=Ucal::Manager::scanBackendDirectory(directory.path());
    QCOMPARE(scan.packages.size(),2); QVERIFY(scan.packages[0].id!=scan.packages[1].id);
    QCOMPARE(scan.packages[0].backendFamily,scan.packages[1].backendFamily);

}

void ManagerModelTest::backendPackagesAreReadOnly() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto previous=qgetenv("UCAL_BACKEND_DIRECTORY");
    struct Restore { QByteArray value; ~Restore(){qputenv("UCAL_BACKEND_DIRECTORY",value);} } restore{previous};
    const auto backends=directory.filePath("Backends"); QVERIFY(QDir().mkpath(backends+"/test"));
    qputenv("UCAL_BACKEND_DIRECTORY",backends.toUtf8());
    QByteArray pe(256,'\0');pe[0]='M';pe[1]='Z';pe[0x3c]=char(0x80);pe[0x80]='P';pe[0x81]='E';pe[0x84]=char(0x64);pe[0x85]=char(0x86);pe[0x97]=char(0x20);
    const auto dll=createFile(directory.filePath("vendor.dll"),pe);
    QJsonObject object{{"schema","org.ucal.backend-package/3"},{"id","test"},{"name","Vendor J2534"},
        {"vendor","Vendor"},{"version","1.0.0"},{"type","j2534-0404-gmw17753"},{"library",dll},
        {"configurationTool",QJsonObject{{"program","missing-config.exe"}}}};
    QVERIFY(!createFile(backends+"/test/backend.json",QJsonDocument(object).toJson()).isEmpty());
    Ucal::Manager::MainWindow window(PresentationMode::Production,UiLanguage::SimplifiedChinese,false,false,directory.filePath("workspace.json"));
    window.selectPage(3); window.show(); QTest::qWait(20);
    QPushButton* details=nullptr;
    for (auto* button:window.findChildren<QPushButton*>()) {
        QVERIFY(button->property("actionId").toString()!=QStringLiteral("addBackendPlugin"));
        QVERIFY(button->objectName()!=QStringLiteral("configurationCapabilities"));
        if(button->property("backendId").toString()==QStringLiteral("test")) details=button;
    }
    QVERIFY(details); saveReviewSnapshot(&window,"backend-packages-readonly");
    bool checked=false;
    QTimer::singleShot(0,&window,[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog)return;
        checked=dialog->findChildren<QLineEdit*>().isEmpty() && dialog->findChildren<QTableWidget*>().isEmpty()
            && dialog->findChildren<QComboBox*>().isEmpty()
            && dialog->findChild<QPushButton*>("openBackendConfigurationTool");
        saveReviewSnapshot(dialog,"backend-package-details"); dialog->reject();
    });
    details->click(); QVERIFY(checked);
}

void ManagerModelTest::backendEnablementPersistsWithoutChangingMetadata() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto oldDirectory = qgetenv("UCAL_BACKEND_DIRECTORY");
    const auto oldPreferences = Ucal::Manager::loadUserPreferences();
    struct Restore { QByteArray directory; Ucal::Manager::UserPreferences preferences; ~Restore() {
        qputenv("UCAL_BACKEND_DIRECTORY", directory); Ucal::Manager::saveUserPreferences(preferences);
    } } restore{oldDirectory, oldPreferences};
    auto preferences = oldPreferences; preferences.disabledBackendIds.clear();
    QVERIFY(Ucal::Manager::saveUserPreferences(preferences));
    const auto folder = directory.filePath("backends/fixture"); QVERIFY(QDir().mkpath(folder));
    // This test toggles catalog membership without executing a worker. Supply its
    // own path fixture instead of accidentally relying on a prior native build.
    QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("runtime/x64"))));
    QVERIFY(!createFile(directory.filePath(QStringLiteral("runtime/x64/UcalBroker.exe"))).isEmpty());
    qputenv("UCAL_BACKEND_DIRECTORY", directory.filePath("backends").toUtf8());
    QByteArray pe(256,'\0'); pe[0]='M';pe[1]='Z';pe[0x3c]=char(0x80);pe[0x80]='P';pe[0x81]='E';pe[0x84]=char(0x64);pe[0x85]=char(0x86);pe[0x97]=char(0x20);
    createFile(folder+"/fixture.dll", pe);
    const auto metadata = QJsonDocument(QJsonObject{{"schema","org.ucal.backend-package/4"},{"id","fixture"},
        {"name","Fixture backend"},{"vendor","Fixture"},{"version","1.0"},{"type","ucal"},
        {"backendFamily","fixture"},{"library","fixture.dll"}}).toJson();
    const auto manifest = createFile(folder+"/backend.json", metadata);
    auto toggle = [](Ucal::Manager::MainWindow& window) -> QPushButton* {
        auto* page = window.findChild<QStackedWidget*>("mainPages")->widget(3);
        for (auto* button : page->findChildren<QPushButton*>())
            if (button->property("backendToggleId").toString() == QStringLiteral("fixture")) return button;
        return nullptr;
    };
    {
        Ucal::Manager::MainWindow window(PresentationMode::Production, UiLanguage::English, false, false, directory.filePath("workspace.json"));
        window.selectPage(3); window.show();
        QVERIFY(toggle(window)); QCOMPARE(toggle(window)->text(),QStringLiteral("Enabled"));
        toggle(window)->click();
        QVERIFY(Ucal::Manager::loadUserPreferences().disabledBackendIds.contains(QStringLiteral("fixture")));
        QCOMPARE(toggle(window)->text(),QStringLiteral("Disabled"));
        QFile catalog(Ucal::Manager::backendPluginCatalogPath()); QVERIFY(catalog.open(QIODevice::ReadOnly));
        const auto bytes = catalog.readAll();
        const auto text = QString::fromUtf16(reinterpret_cast<const ushort*>(bytes.constData()),bytes.size()/2);
        QVERIFY(text.contains(QStringLiteral("count=0")));
        saveReviewSnapshot(&window, QStringLiteral("backend-disabled"));
    }
    {
        Ucal::Manager::MainWindow restarted(PresentationMode::Production, UiLanguage::English, false, false, directory.filePath("workspace.json"));
        QCOMPARE(toggle(restarted)->text(),QStringLiteral("Disabled"));
        toggle(restarted)->click();
        QVERIFY(!Ucal::Manager::loadUserPreferences().disabledBackendIds.contains(QStringLiteral("fixture")));
        QCOMPARE(toggle(restarted)->text(),QStringLiteral("Enabled"));
    }
    QFile file(manifest); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),metadata);
    auto model = ManagerModel::demo();
    auto packages = model.backendPackages();
    for (auto& package : packages) if (package.id == QStringLiteral("mockapi")) package.backendFamily = QStringLiteral("MOCKAPI");
    model.replaceBackendPackages(packages);
    const auto live = model.endpoints();
    const auto id = model.endpointIdForChannel(1);
    QVERIFY(model.setBackendPackageEnabled(QStringLiteral("mockapi"),false));
    QCOMPARE(model.endpointById(id)->state,EndpointState::Disabled);
    QCOMPARE(model.endpointIdForChannel(1),id);
    for (const auto& m : model.fullAccessMappings()) QVERIFY(model.endpointById(m.endpointId)->backendFamily != QStringLiteral("MOCKAPI"));
    QVERIFY(model.setBackendPackageEnabled(QStringLiteral("mockapi"),true));
    QCOMPARE(model.endpointById(id)->state,EndpointState::Offline); // enabling never invents presence
    model.reconcileInventory(live); QCOMPARE(model.endpointById(id)->state,EndpointState::Online);
}

void ManagerModelTest::nativeBackendSettingsAreUserOwned() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto module = directory.filePath(QStringLiteral("backends/fixture"));
    const auto settings = directory.filePath(QStringLiteral("data/backend-plugins"));
    QVERIFY(QDir().mkpath(module));
    QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("runtime/x64"))));
    QVERIFY(!createFile(directory.filePath(QStringLiteral("runtime/x64/UcalBroker.exe"))).isEmpty());
    QByteArray pe(256, '\0'); pe[0]='M'; pe[1]='Z'; pe[0x3c]=char(0x80);
    pe[0x80]='P'; pe[0x81]='E'; pe[0x84]=char(0x64); pe[0x85]=char(0x86); pe[0x97]=char(0x20);
    QVERIFY(!createFile(QDir(module).filePath(QStringLiteral("fixture.dll")), pe).isEmpty());
    const QByteArray original("[runtime]\nlibrary=\"../Vendor runtime.dll\"\n[custom]\nliteral=@value\\path\n");
    const auto legacy = createFile(QDir(module).filePath(QStringLiteral("backend.ini")), original);
    const auto manifest = QDir(module).filePath(QStringLiteral("backend.json"));
    const QJsonObject metadata{{"schema","org.ucal.backend-package/5"},{"id","fixture"},
        {"name","Fixture"},{"vendor","Fixture"},{"version","1.0"},{"type","ucal"},
        {"backend_family","fixture"},{"library","fixture.dll"},{"configuration","backend.ini"}};
    const auto metadataBytes = QJsonDocument(metadata).toJson();
    QVERIFY(!createFile(manifest, metadataBytes).isEmpty());
    Ucal::Manager::BackendPackage package; QString error;
    QVERIFY2(Ucal::Manager::parseBackendPlugin(metadata, manifest, package, error), qPrintable(error));
    QVector<Ucal::Manager::BackendPackage> packages{package};
    QVERIFY2(Ucal::Manager::synchronizeBackendPlugins(packages, directory.path(), settings).isEmpty(), "Settings migration failed");
    const auto managed = QDir(settings).filePath(QStringLiteral("fixture.ini"));
    QCOMPARE(packages[0].configurationPath, managed);
    QFile saved(managed); QVERIFY(saved.open(QIODevice::ReadOnly));
    QTextStream text(&saved); const auto migrated = text.readAll(); saved.close();
    QVERIFY(migrated.contains(QStringLiteral("library=") + QDir::toNativeSeparators(
        QDir::cleanPath(QDir(module).filePath(QStringLiteral("../Vendor runtime.dll"))))));
    QVERIFY(migrated.contains(QStringLiteral("literal=@value\\path")));
    QFile source(legacy); QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), original); source.close();
    const auto overrideBytes = QByteArray("[runtime]\nlibrary=C:\\Users\\Fixture\\chosen.dll\n");
    QVERIFY(!createFile(managed, overrideBytes).isEmpty());
    QVERIFY(!createFile(legacy, QByteArray("[runtime]\nlibrary=changed.dll\n")).isEmpty());
    // A fresh discovery must keep the user's saved override, not reimport the template.
    QVERIFY(Ucal::Manager::parseBackendPlugin(metadata, manifest, package, error)); packages = {package};
    QVERIFY(Ucal::Manager::synchronizeBackendPlugins(packages, directory.path(), settings).isEmpty());
    QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), overrideBytes); saved.close();
    QFile unchanged(manifest); QVERIFY(unchanged.open(QIODevice::ReadOnly)); QCOMPARE(unchanged.readAll(), metadataBytes);
    package.id = QStringLiteral("automatic"); package.configurationTemplatePath = directory.filePath(QStringLiteral("empty.ini"));
    QVERIFY(!createFile(package.configurationTemplatePath, QByteArray("[runtime]\nlibrary=\n")).isEmpty());
    packages = {package}; QVERIFY(Ucal::Manager::synchronizeBackendPlugins(packages, directory.path(), settings).isEmpty());
    QCOMPARE(packages[0].configurationPath, QDir(settings).filePath(QStringLiteral("automatic.ini")));
    QVERIFY(!QFileInfo(packages[0].configurationPath).exists());
    package.id = QStringLiteral("invalid");
    QVERIFY(QDir().mkpath(QDir(settings).filePath(QStringLiteral("invalid.ini"))));
    packages = {package}; QVERIFY(!Ucal::Manager::synchronizeBackendPlugins(packages, directory.path(), settings).isEmpty());
    QVERIFY(!packages[0].loadError.isEmpty());
}

void ManagerModelTest::userPackageRootDoesNotDependOnDeveloperTools() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("manager"))));
    QVERIFY(!createFile(directory.filePath(QStringLiteral("Ucal.sln"))).isEmpty());
    QVERIFY(!createFile(directory.filePath(QStringLiteral("manager/UcalManager.pro"))).isEmpty());
    const auto product = directory.filePath(QStringLiteral("user package"));
    QVERIFY(QDir().mkpath(QDir(product).filePath(QStringLiteral("runtime/x64"))));
    QVERIFY(QDir().mkpath(QDir(product).filePath(QStringLiteral("backends"))));
    QVERIFY(!createFile(QDir(product).filePath(QStringLiteral("runtime/x64/UcalBroker.exe"))).isEmpty());
    QCOMPARE(Ucal::Manager::resolveProductRoot(product), product);
    const auto damaged = directory.filePath(QStringLiteral("damaged package"));
    QVERIFY(QDir().mkpath(damaged));
    QVERIFY(!createFile(QDir(damaged).filePath(QStringLiteral("UcalManagerPackage.json")), QByteArray("{}")).isEmpty());
    QCOMPARE(Ucal::Manager::resolveProductRoot(damaged), damaged);
    QVERIFY(!QFileInfo(QDir(product).filePath(QStringLiteral("tools"))).exists());
}

void ManagerModelTest::hardwareKeywordFilterIsSessionOnly() {
    QTemporaryDir directory;
    const auto workspace = directory.filePath("filter.json");
    {
        Ucal::Manager::MainWindow window(PresentationMode::Explanatory, UiLanguage::English, false, false, workspace);
        window.selectPage(1); window.show(); QApplication::processEvents();
        auto* field = window.findChild<QLineEdit*>(QStringLiteral("hardwareFilter")); QVERIFY(field);
        auto* page = window.findChild<QStackedWidget*>(QStringLiteral("mainPages"))->widget(1);
        const auto visible = [page] {
            QStringList ids;
            for (auto* row : page->findChildren<QFrame*>())
                if (!row->property("hardwareEndpointId").toString().isEmpty() && !row->isHidden()) ids << row->property("hardwareEndpointId").toString();
            return ids;
        };
        QCOMPARE(visible().size(), 7);
        field->setText(QStringLiteral("mOcKaPi 12345 CAN2"));
        QCOMPARE(visible(),QStringList{QStringLiteral("mockapi:demo-12345:can:1")});
        field->setText(QStringLiteral("PEAK 67890")); QCOMPARE(visible().size(),1);
        field->setText(QStringLiteral("no-such-hardware")); QVERIFY(visible().isEmpty());
        QVERIFY(page->findChild<QLabel*>(QStringLiteral("hardwareFilterEmpty"))->isVisible());
        field->clear(); QCOMPARE(visible().size(),7);
        field->setText(QStringLiteral("12345")); saveReviewSnapshot(&window,QStringLiteral("hardware-keyword-filter"));
        QVERIFY(!QFileInfo::exists(workspace));
    }
    Ucal::Manager::MainWindow restarted(PresentationMode::Explanatory, UiLanguage::English, false, false, workspace);
    QVERIFY(restarted.findChild<QLineEdit*>(QStringLiteral("hardwareFilter"))->text().isEmpty());
}

void ManagerModelTest::legacyCustomProjectMembershipIsEditable() {
    QTemporaryDir directory; const auto workspace=directory.filePath("projects.json");
    auto object=ManagerModel::demo().toJson();
    object.insert("projects",QJsonArray{QJsonObject{{"id","default"},{"name","Default"},{"includeAllApplications",true}},
        QJsonObject{{"id","custom-test"},{"name","Custom bench"},{"includeAllApplications",true}}});
    createFile(workspace,QJsonDocument(object).toJson());
    QString excluded;
    {
        Ucal::Manager::MainWindow window(PresentationMode::Explanatory,UiLanguage::English,false,true,workspace);
        bool inspected=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            QTimer::singleShot(10000,dialog,&QDialog::reject);
            auto* projects=dialog->findChild<QListWidget*>("projectList");
            auto* apps=dialog->findChild<QListWidget*>("projectApplicationList"); QVERIFY(projects&&apps);
            projects->setCurrentRow(0); QVERIFY(apps->count()>1); QVERIFY(!(apps->item(0)->flags()&Qt::ItemIsEnabled));
            projects->setCurrentRow(1);
            for(int i=0;i<apps->count();++i){QVERIFY(apps->item(i)->flags()&Qt::ItemIsEnabled);QCOMPARE(apps->item(i)->checkState(),Qt::Checked);}
            excluded=apps->item(0)->data(Qt::UserRole).toString(); apps->item(0)->setCheckState(Qt::Unchecked);
            inspected=true; dialog->accept();
        });
        window.showDialogForReview(QStringLiteral("project")); QVERIFY(inspected);
    }
    ManagerModel saved; QVERIFY(ManagerModel::loadFromFile(workspace,&saved));
    const auto* project=saved.projectById(QStringLiteral("custom-test")); QVERIFY(project);
    QVERIFY(!project->includeAllApplications); QVERIFY(!project->applicationIds.contains(excluded));
    QCOMPARE(saved.applicationsForProject(project->id).size(),saved.applications().size()-1);
    QCOMPARE(saved.applicationsForProject(QStringLiteral("default")).size(),saved.applications().size());
    ManagerModel again; QVERIFY(ManagerModel::fromJson(saved.toJson(),&again));
    QVERIFY(!again.projectById(project->id)->applicationIds.contains(excluded));
}

void ManagerModelTest::packageMetadataAndTools() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto manifest=directory.filePath("backend.json");
    Ucal::Manager::PackageTool tool; QString error;
    QVERIFY(Ucal::Manager::parsePackageTool(QJsonObject{{"program","config tool.exe"},
        {"arguments",QJsonArray{"one two","literal;$()"}}, {"workingDirectory","."}},manifest,tool,error));
    QCOMPARE(tool.program,directory.filePath("config tool.exe"));
    QCOMPARE(tool.arguments,QStringList({"one two","literal;$()"}));
    QVERIFY(!Ucal::Manager::startPackageTool(tool,&error)); QVERIFY(!error.isEmpty());
    QVERIFY(!Ucal::Manager::parsePackageTool(QJsonObject{{"program","x.exe"},{"arguments","/x"}},manifest,tool,error));
    QVERIFY(!Ucal::Manager::parsePackageTool(QJsonObject{{"program","x.exe"},{"unknown",true}},manifest,tool,error));
    // Start a harmless real child and check argument boundaries and working directory.
    QVERIFY(Ucal::Manager::parsePackageTool(QJsonObject{{"program",QCoreApplication::applicationFilePath()},
        {"arguments",QJsonArray{"--package-tool-probe",directory.filePath("tool-result.json"),"one two","literal;$()"}},
        {"workingDirectory",directory.path()}},manifest,tool,error));
    QVERIFY2(Ucal::Manager::startPackageTool(tool,&error),qPrintable(error));
    QTRY_VERIFY(QFileInfo(directory.filePath("tool-result.json")).isFile());
    QFile result(directory.filePath("tool-result.json")); QVERIFY(result.open(QIODevice::ReadOnly));
    const auto recorded=QJsonDocument::fromJson(result.readAll()).object();
    QCOMPARE(recorded.value("argument").toString(),QStringLiteral("one two"));
    QCOMPARE(recorded.value("literal").toString(),QStringLiteral("literal;$()"));
    QCOMPARE(recorded.value("directory").toString(),directory.path());
    QByteArray pe(256,'\0');pe[0]='M';pe[1]='Z';pe[0x3c]=char(0x80);pe[0x80]='P';pe[0x81]='E';pe[0x84]=char(0x64);pe[0x85]=char(0x86);pe[0x97]=char(0x20);
    createFile(directory.filePath("vendor.dll"),pe);
    QJsonObject object{{"schema","org.ucal.backend-package/3"},{"id","test"},{"name","Vendor J2534"},
        {"vendor","Vendor"},{"version","1.0.0"},{"type","j2534-0404-gmw17753"},{"library","vendor.dll"},
        {"channels",QJsonArray{QJsonObject{{"id","can-a"},{"name","CAN 1"},{"selector","opaque-a"},{"physicalPort",0}}}}};
    Ucal::Manager::BackendPackage package;
    QVERIFY2(Ucal::Manager::parseBackendPlugin(object,manifest,package,error),qPrintable(error));
    QVERIFY(package.packageOwnsChannels); QCOMPARE(package.mode,QStringLiteral("j2534"));
    package.configurationPath=directory.filePath("saved.ini");
    auto stale=package.defaultChannels; stale[0].selector="stale-selector";
    QVERIFY(Ucal::Manager::saveBackendPluginChannels(package,stale,&error));
    QCOMPARE(Ucal::Manager::loadBackendPluginChannels(package,&error).first().selector,QStringLiteral("opaque-a"));
    object.insert("schema", "org.ucal.backend-package/4");
    QVERIFY(!Ucal::Manager::parseBackendPlugin(object,manifest,package,error)); // no channel inventory in metadata
    object.remove("channels"); object.insert("discoveryLibrary", "vendor.dll");
    QVERIFY2(Ucal::Manager::parseBackendPlugin(object,manifest,package,error),qPrintable(error));
    QVERIFY(package.runtimeDiscovery); QVERIFY(Ucal::Manager::loadBackendPluginChannels(package).isEmpty());
    const auto folder=directory.filePath("package"); QVERIFY(QDir().mkpath(folder));
    object.insert("library",directory.filePath("vendor.dll")); object.insert("discoveryLibrary",directory.filePath("vendor.dll"));
    createFile(folder+"/backend.json",QJsonDocument(object).toJson());
    const auto scanned=Ucal::Manager::scanBackendDirectory(directory.path());
    QCOMPARE(scanned.packages.size(),1); QVERIFY(scanned.diagnostics.isEmpty());
    object.remove("discoveryLibrary");
    QVERIFY(Ucal::Manager::parseBackendPlugin(object,manifest,package,error));
    QCOMPARE(Ucal::Manager::loadBackendPluginChannels(package).size(), 1); // vendor default logical entry
    object.insert("mode","j2534");
    QVERIFY(!Ucal::Manager::parseBackendPlugin(object,manifest,package,error)); object.remove("mode");
    object.insert("type","j2534-unknown"); QVERIFY(!Ucal::Manager::parseBackendPlugin(object,manifest,package,error));
}

void ManagerModelTest::frontendPackagesAndExternalOperations() {
    QJsonValue normalized;
    QString normalizationError;
    QVERIFY(Ucal::Manager::convertPluginJson(QJsonObject{{"softwareVersion", "12"},
        {"environment", QJsonObject{{"lowerCamelVariable", "unchanged"}}}}, &normalized, true, &normalizationError));
    QVERIFY(normalized.toObject().contains("software_version"));
    QCOMPARE(normalized.toObject().value("environment").toObject().value("lowerCamelVariable").toString(), QStringLiteral("unchanged"));
    QVERIFY(!Ucal::Manager::convertPluginJson(QJsonObject{{"softwareVersion", "12"}, {"software_version", "13"}},
        &normalized, true, &normalizationError));
    QVERIFY(!normalizationError.isEmpty());
    const auto modernPackage = Ucal::Manager::frontendPackage(QStringLiteral("canoe"));
    QVERIFY(!modernPackage.id.isEmpty());
    QLibrary modernLibrary(modernPackage.libraryPath);
    QVERIFY2(modernLibrary.load(), qPrintable(modernLibrary.errorString()));
    const auto getApi = reinterpret_cast<UcalFrontendGetApiFunctionV2>(modernLibrary.resolve("UcalFrontendGetApi"));
    QVERIFY(getApi != nullptr);
    for (const uint32_t abi : {UCAL_FRONTEND_ABI_V1, UCAL_FRONTEND_ABI_V2}) {
        UcalFrontendApiV2 api{}; api.size = sizeof(api);
        QCOMPARE(getApi(abi, &api), int32_t(0));
        QCOMPARE(api.abiVersion, abi);
        const QJsonObject executable{{abi == UCAL_FRONTEND_ABI_V2 ? "executable_path" : "executablePath", QCoreApplication::applicationFilePath()}};
        const auto request = QJsonDocument(QJsonObject{{"plan", QJsonObject{{"software", executable}}}, {"language", 1}}).toJson(QJsonDocument::Compact);
        QByteArray response(UCAL_FRONTEND_MAX_JSON, '\0'); uint32_t written = 0;
        QCOMPARE(api.invoke(UCAL_FRONTEND_PREPARE_LAUNCH, request.constData(), uint32_t(request.size()),
            response.data(), uint32_t(response.size()), &written), int32_t(3));
        const auto result = QJsonDocument::fromJson(response.left(int(written))).object();
        const auto key = abi == UCAL_FRONTEND_ABI_V2 ? QStringLiteral("working_directory") : QStringLiteral("workingDirectory");
        QVERIFY(result.contains(key));
        QVERIFY(!result.value("error").toString().isEmpty());
        QVERIFY(!result.value("error").toString().startsWith("launch."));
    }
    const auto fixture=qEnvironmentVariable("UCAL_FRONTEND_FIXTURE_DLL"); QVERIFY2(QFileInfo(fixture).isFile(),"Build the frontend_plugin_fixture and set UCAL_FRONTEND_FIXTURE_DLL");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    struct Restore {
        QByteArray directory=qgetenv("UCAL_FRONTEND_DIRECTORY"),exe=qgetenv("UCAL_FRONTEND_TEST_EXE"),exe2=qgetenv("UCAL_FRONTEND_TEST_EXE_2");
        ~Restore(){qputenv("UCAL_FRONTEND_DIRECTORY",directory);qputenv("UCAL_FRONTEND_TEST_EXE",exe);qputenv("UCAL_FRONTEND_TEST_EXE_2",exe2);}
    } restore;
    const auto folder=directory.filePath("fixture"); QVERIFY(QDir().mkpath(folder));
    QJsonObject object{{"schema","org.ucal.frontend-package/1"},{"id","fixture"},{"name","Fixture software"},
        {"version","1.0.0"},{"implementation","dll"},{"library",fixture},
        {"operations",QJsonArray{"scan","inspect","check_compatibility","prepare_launch","deploy","restore_deployment"}},
        {"maximumCanChannels",4},{"fullAccess",true}};
    auto write=[&]{return !createFile(folder+"/frontend.json",QJsonDocument(object).toJson()).isEmpty();};
    QVERIFY(write()); qputenv("UCAL_FRONTEND_DIRECTORY",directory.path().toUtf8());
    qputenv("UCAL_FRONTEND_TEST_EXE",QCoreApplication::applicationFilePath().toUtf8());
    const auto other=QDir(qEnvironmentVariable("WINDIR")).filePath("System32/cmd.exe");
    qputenv("UCAL_FRONTEND_TEST_EXE_2",other.toUtf8());
    auto scan=Ucal::Manager::scanFrontendDirectory(directory.path()); QCOMPARE(scan.packages.size(),1); QVERIFY(scan.diagnostics.isEmpty());
    QStringList diagnostics; auto software=Ucal::Manager::discoverInstalledSoftware(&diagnostics);
    QCOMPARE(software.size(),2); QVERIFY(diagnostics.isEmpty()); QVERIFY(software[0].id!=software[1].id);
    QCOMPARE(software[0].maximumCanChannels(),4);
    auto inspected=Ucal::Manager::inspectSoftwareExecutable(QCoreApplication::applicationFilePath(),"Manual");
    QCOMPARE(inspected.family,QStringLiteral("fixture"));
    const auto legacyFixture=qEnvironmentVariable("UCAL_FRONTEND_LEGACY_FIXTURE_DLL");
    QVERIFY2(QFileInfo(legacyFixture).isFile(),"Build the legacy C-entrypoint fixture");
    object.insert("library",legacyFixture); QVERIFY(write());
    diagnostics.clear();
    const auto legacySoftware=Ucal::Manager::discoverInstalledSoftware(&diagnostics);
    QCOMPARE(legacySoftware.size(),2); QVERIFY(diagnostics.isEmpty());
    QCOMPARE(Ucal::Manager::inspectSoftwareExecutable(QCoreApplication::applicationFilePath(),"Manual").family,
             QStringLiteral("fixture"));
    object.insert("library",fixture); QVERIFY(write());
    ManagerModel model=ManagerModel::productionDefaults(); model.reconcileSoftwareInstallations(software);
    QString id; QVERIFY(model.addApplication(software[0].id,"Fixture application",&id));
    auto* application=model.mutableApplicationById(id); QVERIFY(application);
    application->assignmentMode=AssignmentMode::Selected; application->channelCount=1;
    auto endpoint=ManagerModel::demo().endpoints().first(); endpoint.state=EndpointState::Online;
    endpoint.id=QStringLiteral("11111111-1111-4111-8111-111111111111");
    endpoint.adapterId=QStringLiteral("22222222-2222-4222-8222-222222222222");
    endpoint.physicalIndex=0; endpoint.physicalIndexValid=true;
    model.reconcileInventory({endpoint}); application->mappings={{1,endpoint.id}};
    Ucal::Manager::LaunchEnvironment environment;
    const auto plan=LaunchCoordinator::prepare(model,id,true,false,environment);
    QVERIFY2(plan.ready,qPrintable(plan.summary()));
    const auto launch=Ucal::Manager::ProductLauncher::prepare(plan,directory.path(),directory.path());
    QVERIFY2(launch.valid(),qPrintable(launch.error)); QCOMPARE(launch.arguments,QStringList({"--fixture","a b","literal;$()"}));
    QCOMPARE(launch.environment.value("UCAL_FRONTEND_FIXTURE"),QStringLiteral("ready"));
    QVERIFY(Ucal::Manager::invokeFrontend("fixture","deploy").ok());
    const auto failed=Ucal::Manager::invokeFrontend("fixture","restore_deployment",{{"fail",true}});
    QVERIFY(!failed.ok()); QVERIFY(failed.error.contains("87")); QVERIFY(failed.error.contains("code 3"));
    object.insert("operations",QJsonArray{"scan","inspect","check_compatibility","prepare_launch"}); QVERIFY(write());
    QCOMPARE(Ucal::Manager::invokeFrontend("fixture","deploy").code,1);
    object.insert("operations",QJsonArray{"scan","inspect","prepare_launch"}); QVERIFY(write());
    QVERIFY(LaunchCoordinator::prepare(model,id,true,false,environment).ready);
    object.insert("maximumCanChannels",4.5); QVERIFY(write());
    scan=Ucal::Manager::scanFrontendDirectory(directory.path()); QVERIFY(scan.packages.isEmpty()); QVERIFY(!scan.diagnostics.isEmpty());
    object.insert("maximumCanChannels",4); object.insert("unknown",true); QVERIFY(write());
    QVERIFY(Ucal::Manager::scanFrontendDirectory(directory.path()).packages.isEmpty());
    object.remove("unknown"); object.insert("library","missing.dll"); QVERIFY(write());
    QVERIFY(Ucal::Manager::scanFrontendDirectory(directory.path()).packages.isEmpty());
    QVERIFY(QFile::remove(folder+"/frontend.json")); model.reconcileSoftwareInstallations(Ucal::Manager::discoverInstalledSoftware());
    QVERIFY(!model.softwareById(software[0].id)->available); QVERIFY(model.applicationById(id));
}

void ManagerModelTest::backendFolderScanIsDeterministicAndFailClosed() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto write = [&](const QString& folder, const QByteArray& bytes) {
        QDir().mkpath(directory.filePath(folder));
        QFile file(directory.filePath(folder + QStringLiteral("/backend.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
    };
    const auto library = qEnvironmentVariable("UCAL_FRONTEND_FIXTURE_DLL");
    QVERIFY(QFileInfo(library).isFile());
    const QJsonObject description{{"schema", "org.ucal.backend-package/3"}, {"id", "vector-xl"},
        {"name", "Concrete driver"}, {"vendor", "Example"}, {"version", "1.0.0"},
        {"type", "ucal"}, {"backendFamily", "example-family"}, {"library", library}};
    const QByteArray valid = QJsonDocument(description).toJson();
    write(QStringLiteral("a-vector"), valid);
    write(QStringLiteral("b-duplicate"), valid);
    write(QStringLiteral("c-invalid"), QByteArray("bad json"));
    write(QStringLiteral("d-unknown"), QByteArray(valid).replace("org.ucal.backend-package/3", "org.ucal.backend-package/1"));
    const auto scan = Ucal::Manager::scanBackendDirectory(directory.path());
    QCOMPARE(scan.packages.size(), 1);
    QCOMPARE(scan.packages.first().id, QStringLiteral("vector-xl"));
    QCOMPARE(scan.diagnostics.size(), 3);
    ManagerModel model = ManagerModel::productionDefaults();
    model.replaceBackendPackages(scan.packages);
    QCOMPARE(model.backendPackages().size(), 1);
    model.replaceBackendPackages(Ucal::Manager::scanBackendDirectory(directory.filePath(QStringLiteral("absent"))).packages);
    QVERIFY(model.backendPackages().isEmpty());
}

void ManagerModelTest::backendRescanPreservesMissingPackagesAndMappings() {
    ManagerModel model = ManagerModel::realisticDemo();
    const auto workspace = model.toJson();
    auto packages = model.backendPackages();
    QVERIFY(packages.size() >= 2);
    const QString missingId = packages.first().id;
    const QString updatedId = packages.at(1).id;
    packages.removeFirst();
    packages[0].version = QStringLiteral("9.9.9");
    auto added = packages.first();
    added.id = QStringLiteral("new-package");
    packages.push_back(added);
    model.reconcileBackendPackages(packages);
    QCOMPARE(model.backendPackages().first().id, missingId);
    QVERIFY(!model.backendPackageById(missingId)->manifestAvailable);
    QCOMPARE(model.backendPackageById(updatedId)->version, QStringLiteral("9.9.9"));
    QCOMPARE(model.backendPackages().last().id, added.id);
    QCOMPARE(model.toJson(), workspace);
    auto restored = *model.backendPackageById(missingId);
    restored.manifestAvailable = true;
    packages.push_back(restored);
    model.reconcileBackendPackages(packages);
    QVERIFY(model.backendPackageById(missingId)->manifestAvailable);
    QCOMPARE(model.backendPackages().first().id, missingId);
    QCOMPARE(model.toJson(), workspace);
}

void ManagerModelTest::explicitChannelEditsStayWithinSoftwareLimits() {
    ManagerModel model = ManagerModel::realisticDemo();
    const auto applicationId = model.applications().first().id;
    QVERIFY(model.endpoints().size() >= 3);
    const QVector<QString> original{model.endpoints().at(0).id,
        model.endpoints().at(1).id, model.endpoints().at(2).id};
    QVERIFY(model.replaceApplicationChannels(applicationId, original));
    const QVector<QString> reduced{original.at(0), original.at(2)};
    QVERIFY(model.replaceApplicationChannels(applicationId, reduced));
    const auto* application = model.applicationById(applicationId);
    QCOMPARE(application->channelCount, 2);
    QCOMPARE(application->mappings.size(), 2);
    QCOMPARE(application->mappings.at(1).logicalChannel, 2);
    QCOMPARE(application->mappings.at(1).endpointId, original.at(2));
    QVERIFY(!model.replaceApplicationChannels(applicationId, {}));
    QVERIFY(!model.replaceApplicationChannels(applicationId, QVector<QString>(17)));
    QVERIFY(!model.replaceApplicationChannels(applicationId, {QStringLiteral("unknown-endpoint")}));
    model.reconcileInventory({});
    ManagerModel restored;
    QVERIFY(ManagerModel::fromJson(model.toJson(), &restored));
    const auto* saved = restored.applicationById(applicationId);
    QCOMPARE(saved->channelCount, 2);
    QCOMPARE(saved->mappings.size(), 2);
    QCOMPARE(saved->mappings.at(1).endpointId, original.at(2));
    for (const auto& software : model.softwareInstallations()) {
        if (software.family != QStringLiteral("pcanview")) continue;
        QCOMPARE(software.maximumCanChannels(), 1);
        QString id;
        QVERIFY(model.addApplication(software.id, QStringLiteral("One channel"), &id));
        QVERIFY(!model.applicationById(id)->createdAtUtc.isEmpty());
        QVERIFY(!model.replaceApplicationChannels(id, reduced));
        QVERIFY(model.replaceApplicationChannels(id, {original.first()}));
    }
}

void ManagerModelTest::launchArgumentsRoundTripWithWindowsQuoting() {
    ManagerModel model = ManagerModel::realisticDemo();
    auto* app = model.mutableApplicationById(model.applications().first().id);
    QVERIFY(app);
    app->commandLineArguments = QStringLiteral("/flag /title \"two words\" \"C:\\test folder\\\\\" \"\"");
    ManagerModel restored;
    QVERIFY(ManagerModel::fromJson(model.toJson(), &restored));
    QCOMPARE(restored.applications().first().commandLineArguments, app->commandLineArguments);
    Ucal::Manager::LaunchPlan plan;
    plan.commandLineArguments = app->commandLineArguments;
    const auto args = Ucal::Manager::applicationArguments(plan);
    QCOMPARE(args, QStringList({QStringLiteral("/flag"), QStringLiteral("/title"),
        QStringLiteral("two words"), QStringLiteral("C:\\test folder\\"), QString()}));
}

void ManagerModelTest::legacyWorkspaceDoesNotRestoreDriverCatalog() {
    ManagerModel model = ManagerModel::realisticDemo();
    auto workspace = model.toJson();
    workspace.insert(QStringLiteral("backendPackages"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("obsolete-registration")}}});
    ManagerModel restored;
    QVERIFY(ManagerModel::fromJson(workspace, &restored));
    QVERIFY(restored.backendPackages().isEmpty());
    QCOMPARE(restored.applications().size(), model.applications().size());
    QCOMPARE(restored.applications().first().mappings.size(), model.applications().first().mappings.size());
    QVERIFY(!restored.toJson().contains(QStringLiteral("backendPackages")));
}

void ManagerModelTest::vxlProductAssetsAndBackupsAreBounded() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString script = directory.filePath(
        QStringLiteral("frontends/vxlapi/tools/Set-UcalVxlClientDeployment.ps1"));
    QVERIFY(QDir().mkpath(QFileInfo(script).absolutePath()));
    QVERIFY(!createFile(script).isEmpty());
    const QString x64Root = directory.filePath(QStringLiteral("runtime/x64/frontends"));
    QVERIFY(QDir().mkpath(x64Root));
    QVERIFY(!createFile(QDir(x64Root).filePath(QStringLiteral("vxlapi64.dll"))).isEmpty());
    QVERIFY(!createFile(QDir(x64Root).filePath(
                            QStringLiteral("UcalXlDiscovery64.dll"))).isEmpty());
    const auto x64 = Ucal::Manager::resolveVxlProductAssets(directory.path(),
                                                         QStringLiteral("x64"));
    QVERIFY2(x64.ready(), qPrintable(x64.error));
    QVERIFY(!Ucal::Manager::resolveVxlProductAssets(
                 directory.path(), QStringLiteral("x86")).ready());
    QVERIFY(!Ucal::Manager::resolveVxlProductAssets(
                 directory.path(), QStringLiteral("arm64")).ready());

    const QString workspace = directory.filePath(QStringLiteral("profiles.json"));
    const QString first = directory.filePath(
        QStringLiteral("deployments/app-id/20260101T000000000Z/deployment.json"));
    QVERIFY(QDir().mkpath(QFileInfo(first).absolutePath()));
    QVERIFY(!createFile(first, QByteArray("{}" )).isEmpty());
    QTest::qSleep(1100);
    const QString second = directory.filePath(
        QStringLiteral("deployments/app-id/20260101T000001000Z/deployment.json"));
    QVERIFY(QDir().mkpath(QFileInfo(second).absolutePath()));
    QVERIFY(!createFile(second, QByteArray("{}" )).isEmpty());
    QCOMPARE(QDir::cleanPath(Ucal::Manager::latestVxlDeploymentBackup(
                 workspace, QStringLiteral("app-id"))),
             QDir::cleanPath(QFileInfo(second).absolutePath()));
    const QString next = QDir::cleanPath(Ucal::Manager::nextVxlDeploymentBackup(
        workspace, QStringLiteral("../app/id")));
    const QString deploymentRoot = QDir::cleanPath(
        directory.filePath(QStringLiteral("deployments")));
    QVERIFY(next.startsWith(deploymentRoot + QStringLiteral("/")));
}

void ManagerModelTest::projectViewsDoNotOwnApplications() {
    ManagerModel model = ManagerModel::demo();
    QCOMPARE(model.applicationsForProject(QStringLiteral("default")).size(), model.applications().size());
    QCOMPARE(model.applicationsForProject(QStringLiteral("powertrain")).size(), 2);

    const int applicationCount = model.applications().size();
    QVERIFY(model.removeProject(QStringLiteral("powertrain")));
    QCOMPARE(model.applications().size(), applicationCount);
    QVERIFY(!model.removeProject(QStringLiteral("default")));
}

void ManagerModelTest::multipleApplicationsCanShareSoftware() {
    ManagerModel model = ManagerModel::demo();
    const auto* laboratory = model.applicationById(QStringLiteral("canoe-lab"));
    const auto* calibration = model.applicationById(QStringLiteral("canoe-cal"));
    QVERIFY(laboratory != nullptr);
    QVERIFY(calibration != nullptr);
    QCOMPARE(laboratory->softwareInstallationId, calibration->softwareInstallationId);
    QVERIFY(laboratory->id != calibration->id);
}

void ManagerModelTest::assignmentModesStayExplicit() {
    ManagerModel model = ManagerModel::demo();
    auto* calibration = model.mutableApplicationById(QStringLiteral("canoe-cal"));
    QVERIFY(calibration != nullptr);
    QCOMPARE(calibration->assignmentMode, AssignmentMode::FullAccess);
    QVERIFY(model.setApplicationChannelCount(calibration->id, 4));
    QVERIFY(model.setApplicationMapping(calibration->id, 1, model.endpoints().first().id));
    QCOMPARE(calibration->assignmentMode, AssignmentMode::Selected);
    QCOMPARE(calibration->channelCount, 4);
    QCOMPARE(calibration->mappings.first().endpointId, model.endpoints().first().id);
}

void ManagerModelTest::endpointAssignmentUsesFreeSlotsWithoutOverwrite() {
    ManagerModel model = ManagerModel::demo();
    const QString applicationId = QStringLiteral("canoe-lab");
    QVERIFY(model.replaceApplicationChannels(applicationId,
        {model.endpoints().at(1).id, model.endpoints().at(2).id}));
    const QString addedEndpoint = model.endpoints().first().id;
    QVERIFY(model.setApplicationEndpointAssigned(applicationId, addedEndpoint, true));
    const auto* application = model.applicationById(applicationId);
    QVERIFY(application != nullptr);
    QCOMPARE(application->channelCount, 3);
    QCOMPARE(application->mappings.at(2).endpointId, addedEndpoint);

    QVERIFY(model.setApplicationEndpointAssigned(applicationId, addedEndpoint, false));
    application = model.applicationById(applicationId);
    QVERIFY(application != nullptr);
    QCOMPARE(application->channelCount, 3);
    QVERIFY(application->mappings.at(2).endpointId.isEmpty());

    QVector<Endpoint> capacityEndpoints;
    for (int index = 0; index < 17; ++index) {
        auto endpoint = model.endpoints().first();
        endpoint.id = QStringLiteral("capacity-endpoint-%1").arg(index);
        capacityEndpoints.push_back(endpoint);
    }
    model.reconcileInventory(capacityEndpoints);
    QVERIFY(model.replaceApplicationChannels(applicationId, QVector<QString>(16)));
    for (int channel = 1; channel <= 16; ++channel) {
        QVERIFY(model.setApplicationMapping(applicationId, channel, capacityEndpoints.at(channel - 1).id));
    }
    const QString rejectedEndpoint = capacityEndpoints.last().id;
    QVERIFY(!model.setApplicationEndpointAssigned(applicationId, rejectedEndpoint, true));
    application = model.applicationById(applicationId);
    QVERIFY(application != nullptr);
    QCOMPARE(application->channelCount, 16);
    QCOMPARE(application->mappings.at(15).endpointId, capacityEndpoints.at(15).id);

    QVERIFY(!model.setApplicationEndpointAssigned(
        QStringLiteral("canoe-cal"), addedEndpoint, true));
}

void ManagerModelTest::manualSoftwareCreatesAnIndependentInstallation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = directory.filePath(QStringLiteral("CompanyCalibration.exe"));
    QFile file(executable);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("POC");
    file.close();

    ManagerModel model = ManagerModel::demo();
    const int before = model.softwareInstallations().size();
    const QString installationId = model.addManualSoftware(executable);
    QVERIFY(!installationId.isEmpty());
    QCOMPARE(model.softwareInstallations().size(), before + 1);
    const auto* installation = model.softwareById(installationId);
    QVERIFY(installation != nullptr);
    QCOMPARE(installation->source, QStringLiteral("Manual"));
    QCOMPARE(installation->compatibilityKind, QStringLiteral("danger"));
    QVERIFY(installation->integrationAdapters.isEmpty());

    QString applicationId;
    QVERIFY(!model.addApplication(installationId, QStringLiteral("公司标定工具"), &applicationId));
    QVERIFY(applicationId.isEmpty());
}

void ManagerModelTest::manualSoftwareCompatibilityUsesExistingRoutes() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString unknownExecutable =
        createFile(directory.filePath(QStringLiteral("CompanyCalibration.exe")));

    ManagerModel model = ManagerModel::productionDefaults();
    const QString unknownId = model.addManualSoftware(unknownExecutable);
    QVERIFY(!unknownId.isEmpty());
    QCOMPARE(model.addManualSoftware(unknownExecutable), unknownId);
    QCOMPARE(model.softwareInstallations().size(), 1);
    const auto* unknown = model.softwareById(unknownId);
    QVERIFY(unknown != nullptr);
    QCOMPARE(unknown->family, QStringLiteral("custom"));
    QVERIFY(unknown->integrationAdapters.isEmpty());
    QCOMPARE(unknown->compatibilityKind, QStringLiteral("danger"));

    const QString canoeExecutable =
        directory.filePath(QStringLiteral("CANoe64.exe"));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), canoeExecutable));
    const Ucal::Manager::SoftwareInstallation canoe =
        Ucal::Manager::inspectSoftwareExecutable(canoeExecutable);
    QCOMPARE(canoe.family, QStringLiteral("canoe"));
    QVERIFY(!canoe.integrationAdapters.contains(QStringLiteral("vxl-process-route")));
    QCOMPARE(canoe.compatibilityKind, QStringLiteral("warning"));
    const QString canoeId = model.addManualSoftware(canoeExecutable);
    QVERIFY(!canoeId.isEmpty());
    model.reconcileSoftwareInstallations({});
    const auto* retainedCanoe = model.softwareById(canoeId);
    QVERIFY(retainedCanoe != nullptr);
    QVERIFY(retainedCanoe->available);
    QCOMPARE(retainedCanoe->family, QStringLiteral("canoe"));

    QVERIFY(QFile::remove(unknownExecutable));
    QVERIFY(model.reinspectManualSoftware(unknownId));
    unknown = model.softwareById(unknownId);
    QVERIFY(unknown != nullptr);
    QVERIFY(!unknown->available);
    QVERIFY(unknown->integrationAdapters.isEmpty());
}

void ManagerModelTest::realisticDemoUsesProductFacingStates() {
    const ManagerModel model = ManagerModel::realisticDemo();
    QVERIFY(!model.applications().isEmpty());
    for (const auto& application : model.applications()) {
        QVERIFY(application.statusKind != QStringLiteral("success"));
        QVERIFY(application.maturity != QStringLiteral("Ready"));
    }
    for (const auto& software : model.softwareInstallations()) {
        QVERIFY(software.compatibility != QStringLiteral("Ready"));
    }
    for (const auto& backend : model.backendPackages()) {
        QVERIFY(backend.maturity != QStringLiteral("Ready"));
    }
    const auto pcanEndpoint = std::find_if(
        model.endpoints().cbegin(), model.endpoints().cend(), [](const auto& endpoint) {
            return endpoint.backendFamily == QStringLiteral("PCAN-Basic");
        });
    QVERIFY(pcanEndpoint != model.endpoints().cend());
    QVERIFY(pcanEndpoint->maturity.contains(QStringLiteral("Bounded physical")));
    QVERIFY(pcanEndpoint->capabilities.contains(QStringLiteral("listen-only")));
    QVERIFY(!pcanEndpoint->capabilities.contains(QStringLiteral("120")));

    const auto pcanBackend = std::find_if(
        model.backendPackages().cbegin(), model.backendPackages().cend(),
        [](const auto& backend) { return backend.id == QStringLiteral("pcan-basic"); });
    QVERIFY(pcanBackend != model.backendPackages().cend());
    QVERIFY(pcanBackend->maturity.contains(QStringLiteral("Bounded physical")));
}

void ManagerModelTest::productionDefaultsContainNoDemoInventory() {
    const ManagerModel model = ManagerModel::productionDefaults();
    QVERIFY(model.endpoints().isEmpty());
    QVERIFY(model.softwareInstallations().isEmpty());
    QVERIFY(model.applications().isEmpty());
    QVERIFY(model.backendPackages().isEmpty());
    QCOMPARE(model.activeProjectId(), QStringLiteral("default"));
    QVERIFY(ManagerModel::productionStoragePath().endsWith(QStringLiteral("profiles.json")));
}

void ManagerModelTest::softwareReconciliationPreservesMissingInstallation() {
    ManagerModel model = ManagerModel::productionDefaults();
    Ucal::Manager::SoftwareInstallation canoe{
        QStringLiteral("canoe:test"), QStringLiteral("canoe"), QStringLiteral("Vector CANoe"),
        QStringLiteral("12.0.75"), QStringLiteral("x64"), QStringLiteral("C:/CANoe64.exe"),
        QStringLiteral("Windows Registry"), QStringLiteral("Engineering route"),
        QStringLiteral("warning"), {QStringLiteral("canoe-provider")}, true};
    model.reconcileSoftwareInstallations({canoe});
    QCOMPARE(model.softwareInstallations().size(), 1);
    QVERIFY(model.softwareInstallations().first().available);

    model.reconcileSoftwareInstallations({});
    QCOMPARE(model.softwareInstallations().size(), 1);
    QVERIFY(!model.softwareInstallations().first().available);
    QCOMPARE(model.softwareInstallations().first().id, QStringLiteral("canoe:test"));
}

void ManagerModelTest::pcanViewVersionProfilesFailClosed() {
    const QString adapter = QStringLiteral("pcanview5-6-structural-adapter");
    QVERIFY(Ucal::Manager::compatiblePcanViewIntegrationAdapters(
                QStringLiteral("5.0.2.838"), QStringLiteral("x64")).contains(adapter));
    QVERIFY(Ucal::Manager::compatiblePcanViewIntegrationAdapters(
                QStringLiteral("6.0.0.1040"), QStringLiteral("x64")).contains(adapter));
    QVERIFY(!Ucal::Manager::compatiblePcanViewIntegrationAdapters(
                 QStringLiteral("5.0.2.838"), QStringLiteral("x86")).contains(adapter));
    QVERIFY(!Ucal::Manager::compatiblePcanViewIntegrationAdapters(
                 QStringLiteral("4.2.4.564"), QStringLiteral("x86")).contains(adapter));
    QVERIFY(!Ucal::Manager::compatiblePcanViewIntegrationAdapters(
                 QStringLiteral("4.2.4.564"), QStringLiteral("x64")).contains(adapter));
    QVERIFY(!Ucal::Manager::compatiblePcanViewIntegrationAdapters(
                 QStringLiteral("7.0.0.0"), QStringLiteral("x64")).contains(adapter));
}

void ManagerModelTest::translationCatalogSupportsRequiredLanguages() {
    QCOMPARE(Ucal::Manager::uiText(UiLanguage::English, QStringLiteral("nav.applications")),
             QStringLiteral("Applications"));
    QCOMPARE(Ucal::Manager::uiText(UiLanguage::SimplifiedChinese, QStringLiteral("nav.applications")),
             QStringLiteral("应用"));
    QCOMPARE(Ucal::Manager::uiText(UiLanguage::German, QStringLiteral("nav.applications")),
             QStringLiteral("Anwendungen"));
    QCOMPARE(Ucal::Manager::uiText(UiLanguage::Japanese, QStringLiteral("nav.applications")),
             QStringLiteral("アプリ"));
    const QStringList requiredKeys = {
        QStringLiteral("nav.hardware"), QStringLiteral("nav.software"),
        QStringLiteral("nav.backends"), QStringLiteral("nav.settings"),
        QStringLiteral("settings.language"), QStringLiteral("editor.integration"),
        QStringLiteral("project.managerTitle"), QStringLiteral("addApp.title"),
        QStringLiteral("hardware.assignTitle"), QStringLiteral("tray.exitQuestion"),
        QStringLiteral("apps.startAllQueued"), QStringLiteral("software.compat.unknown"),
        QStringLiteral("software.rescanComplete"), QStringLiteral("common.revalidate"),
        QStringLiteral("backends.registrationSummary"),
        QStringLiteral("status.inventoryRefreshed"), QStringLiteral("vxl.explanation"),
        QStringLiteral("vxl.transactionFailed"), QStringLiteral("vxl.action.restore"),
        QStringLiteral("launch.summary.blockers"),
        QStringLiteral("launch.blocker.inventoryStale"),
        QStringLiteral("launch.blocker.routeUnsupported"),
        QStringLiteral("launch.product.canoeRouteIncomplete"),
        QStringLiteral("launch.product.pcanSlotsExhausted"),
        QStringLiteral("editor.route.canoeProviderCandidate"),
        QStringLiteral("editor.route.vxlProcess")};
    for (const QString& key : requiredKeys) {
        for (const UiLanguage language : {UiLanguage::English, UiLanguage::SimplifiedChinese,
                                          UiLanguage::German, UiLanguage::Japanese}) {
            const QString translated = Ucal::Manager::profileText(language, key);
            QVERIFY2(!translated.startsWith(QLatin1Char('[')), qPrintable(key));
            QVERIFY2(!translated.contains(QChar::ReplacementCharacter), qPrintable(key));
        }
    }
    QCOMPARE(Ucal::Manager::uiText(UiLanguage::SimplifiedChinese,
                                QStringLiteral("common.revalidate")),
             QStringLiteral("重新校验"));
    QCOMPARE(Ucal::Manager::uiText(UiLanguage::Japanese,
                                QStringLiteral("common.revalidate")),
             QStringLiteral("再検証"));
    const auto englishHelp = Ucal::Manager::uiText(UiLanguage::English,
                                                  QStringLiteral("hardware.bindingHelp"));
    for (const auto language : {UiLanguage::SimplifiedChinese, UiLanguage::German, UiLanguage::Japanese}) {
        const auto help = Ucal::Manager::uiText(language, QStringLiteral("hardware.bindingHelp"));
        QVERIFY(!help.isEmpty());
        QVERIFY(help != englishHelp);
        QVERIFY(!help.startsWith(QLatin1Char('[')));
    }
}

void ManagerModelTest::startupWindowFitsAvailableDesktop() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    Ucal::Manager::MainWindow window(PresentationMode::Realistic, UiLanguage::German,
                                    false, false, workspace.filePath("profiles.json"));
    window.selectPage(4);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto* screen = QGuiApplication::screenAt(window.frameGeometry().center());
    QVERIFY(screen != nullptr);
    QVERIFY2(screen->availableGeometry().contains(window.frameGeometry()),
             qPrintable(QStringLiteral("Window %1,%2 %3x%4 exceeds desktop %5x%6")
                 .arg(window.frameGeometry().x()).arg(window.frameGeometry().y())
                 .arg(window.frameGeometry().width()).arg(window.frameGeometry().height())
                 .arg(screen->availableGeometry().width()).arg(screen->availableGeometry().height())));
    for (int page = 0; page < 5; ++page) {
        window.selectPage(page);
        QCoreApplication::processEvents();
        // Off-viewport rows are deliberately scrollable. Controls outside a
        // scroll area (navigation, filters and primary header actions) must fit.
        for (auto* control : window.findChildren<QWidget*>()) {
            if (!qobject_cast<QAbstractButton*>(control) && !qobject_cast<QLineEdit*>(control)) continue;
            if (!control->isVisibleTo(&window)) continue;
            bool scrollable = false;
            for (auto* parent = control->parentWidget(); parent; parent = parent->parentWidget())
                scrollable = scrollable || qobject_cast<QAbstractScrollArea*>(parent) != nullptr;
            if (scrollable) continue;
            const auto bounds = QRect(control->mapTo(&window, QPoint()), control->size());
            QVERIFY2(window.rect().contains(bounds), qPrintable(control->objectName()));
        }
    }
    window.selectPage(4);
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("settingsScrollArea"));
    QVERIFY(scroll != nullptr);
    auto* language = scroll->findChild<QComboBox*>(QStringLiteral("uiLanguagePreference"));
    QVERIFY(language != nullptr);
    scroll->ensureWidgetVisible(language);
    QCoreApplication::processEvents();
    QVERIFY(language->isVisibleTo(scroll->viewport()));
    QVERIFY(scroll->viewport()->rect().intersects(QRect(language->mapTo(scroll->viewport(), QPoint()), language->size())));
}

void ManagerModelTest::presentationModeSupportsNamedArtifacts() {
    QCOMPARE(Ucal::Manager::presentationModeFromArguments({}, QStringLiteral("UcalManager-realistic-poc.exe")),
             PresentationMode::Realistic);
    QCOMPARE(Ucal::Manager::presentationModeFromArguments({}, QStringLiteral("UcalManager-explanatory-poc.exe")),
             PresentationMode::Explanatory);
    QCOMPARE(Ucal::Manager::presentationModeFromArguments(
                 {QStringLiteral("UcalManager.exe"), QStringLiteral("--presentation"), QStringLiteral("realistic")},
                  QStringLiteral("UcalManager.exe")), PresentationMode::Realistic);
    QCOMPARE(Ucal::Manager::presentationModeFromArguments({}, QStringLiteral("UcalManager.exe")),
             PresentationMode::Production);
    QCOMPARE(Ucal::Manager::presentationModeKey(PresentationMode::Production),
             QStringLiteral("production"));
}

void ManagerModelTest::languagePreferenceRoundTrips() {
    QStandardPaths::setTestModeEnabled(true);
    const QString preferencePath = Ucal::Manager::uiLanguagePreferencePath();
    QFile::remove(preferencePath);

    QCOMPARE(Ucal::Manager::loadUiLanguagePreference(), UiLanguage::System);
    QVERIFY(Ucal::Manager::saveUiLanguagePreference(UiLanguage::Japanese));
    QCOMPARE(Ucal::Manager::loadUiLanguagePreference(), UiLanguage::Japanese);
    QVERIFY(Ucal::Manager::saveUiLanguagePreference(UiLanguage::SimplifiedChinese));
    QCOMPARE(Ucal::Manager::loadUiLanguagePreference(), UiLanguage::SimplifiedChinese);

    QFile::remove(preferencePath);
}

void ManagerModelTest::userPreferencesRoundTrip() {
    QStandardPaths::setTestModeEnabled(true);
    const QString preferencePath = Ucal::Manager::uiLanguagePreferencePath();
    QFile::remove(preferencePath);

    Ucal::Manager::UserPreferences preferences;
    preferences.autoStart = true;
    preferences.startMinimized = true;
    preferences.autoScan = false;
    preferences.closeBehavior = Ucal::Manager::CloseBehavior::Ask;
    QVERIFY(Ucal::Manager::saveUserPreferences(preferences));

    const Ucal::Manager::UserPreferences restored = Ucal::Manager::loadUserPreferences();
    QVERIFY(restored.autoStart);
    QVERIFY(restored.startMinimized);
    QVERIFY(!restored.autoScan);
    QCOMPARE(restored.closeBehavior, Ucal::Manager::CloseBehavior::Ask);
    QFile::remove(preferencePath);
}

void ManagerModelTest::startMinimizedPolicyIsBounded() {
    Ucal::Manager::UserPreferences preferences;
    preferences.startMinimized = true;
    QVERIFY(Ucal::Manager::shouldStartMinimized(preferences, true, false, true, false));
    QVERIFY(!Ucal::Manager::shouldStartMinimized(preferences, false, false, true, false));
    QVERIFY(!Ucal::Manager::shouldStartMinimized(preferences, true, true, true, false));
    QVERIFY(!Ucal::Manager::shouldStartMinimized(preferences, true, false, false, false));

    preferences.startMinimized = false;
    QVERIFY(!Ucal::Manager::shouldStartMinimized(preferences, true, false, true, false));
    QVERIFY(Ucal::Manager::shouldStartMinimized(preferences, true, false, true, true));
    QVERIFY(!Ucal::Manager::shouldStartMinimized(preferences, true, true, true, true));
}

void ManagerModelTest::profileShortcutTargetsSwitchLauncher() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = createFile(directory.filePath(QStringLiteral("UcalManager.exe")));
    const QString shortcut = directory.filePath(QStringLiteral("CANoe UCAL.lnk"));
    QString error;
    QVERIFY2(Ucal::Manager::createProfileShortcut(
                 executable, QStringLiteral("application-test"), QStringLiteral("CANoe test"),
                 shortcut, &error), qPrintable(error));
    const QFileInfo created(shortcut);
    QVERIFY(created.isFile());
    QVERIFY(created.size() > 0);
    QString target;
    QString arguments;
    QVERIFY2(Ucal::Manager::readProfileShortcut(shortcut, &target, &arguments, &error),
             qPrintable(error));
    QCOMPARE(QFileInfo(target).absoluteFilePath(), QFileInfo(executable).absoluteFilePath());
    QCOMPARE(arguments, QStringLiteral("--profile \"application-test\""));
}

void ManagerModelTest::profileShortcutLaunchesWithWorkspace() {
#ifdef Q_OS_WIN
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/UCAL shortcut spaces-XXXXXX"));
    QVERIFY(directory.isValid());
    const QString marker = directory.filePath(QStringLiteral("workspace marker.json"));
    const QString shortcut = directory.filePath(QStringLiteral("Launch profile.lnk"));
    QString error;
    QVERIFY2(Ucal::Manager::createProfileShortcut(QCoreApplication::applicationFilePath(),
        QStringLiteral("shortcut-launch-probe"), QStringLiteral("Test profile"),
        shortcut, &error, marker), qPrintable(error));
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"open";
    const auto nativeShortcut = QDir::toNativeSeparators(shortcut).toStdWString();
    launch.lpFile = nativeShortcut.c_str();
    launch.nShow = SW_HIDE;
    QVERIFY(ShellExecuteExW(&launch));
    const HANDLE process = launch.hProcess;
    if (process != nullptr) CloseHandle(process);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(marker).size() > 0, 10000);
    QFile result(marker);
    QVERIFY(result.open(QIODevice::ReadOnly));
    const auto object = QJsonDocument::fromJson(result.readAll()).object();
    QCOMPARE(object.value(QStringLiteral("profile")).toString(), QStringLiteral("shortcut-launch-probe"));
    QCOMPARE(QDir::fromNativeSeparators(object.value(QStringLiteral("workspace")).toString()), marker);
    QCOMPARE(QDir::cleanPath(object.value(QStringLiteral("directory")).toString()),
             QDir::cleanPath(QCoreApplication::applicationDirPath()));
#else
    QSKIP("Windows shell links require Windows.");
#endif
}

void ManagerModelTest::profileShortcutRoutesIntoProductInstance() {
#ifdef Q_OS_WIN
    const QString executable = qEnvironmentVariable("UCAL_MANAGER_ACCEPTANCE_EXE");
    if (executable.isEmpty()) QSKIP("Set UCAL_MANAGER_ACCEPTANCE_EXE for built-product shortcut acceptance.");
    QVERIFY(QFileInfo(executable).isFile());
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/UCAL product shortcut-XXXXXX"));
    QVERIFY(directory.isValid());
    const auto workspace = directory.filePath(QStringLiteral("isolated workspace.json"));
    Ucal::Manager::SingleInstanceController primary;
    QString error;
    QVERIFY2(primary.acquirePrimary(workspace, &error), qPrintable(error));
    QSignalSpy requests(&primary, &Ucal::Manager::SingleInstanceController::requestReceived);
    const auto shortcut = directory.filePath(QStringLiteral("Product profile.lnk"));
    QVERIFY2(Ucal::Manager::createProfileShortcut(executable, QStringLiteral("product-profile"),
        QStringLiteral("Product test"), shortcut, &error, workspace, executable), qPrintable(error));
    QString target, arguments, iconPath;
    QVERIFY2(Ucal::Manager::readProfileShortcut(shortcut, &target, &arguments, &error, &iconPath), qPrintable(error));
    QVERIFY(iconPath.endsWith(QStringLiteral(".ico")));
    QVERIFY(QFileInfo(iconPath).isFile());
    QVERIFY(QDir::fromNativeSeparators(iconPath).startsWith(directory.path()));
    QFile iconFile(iconPath); QVERIFY(iconFile.open(QIODevice::ReadOnly));
    QCOMPARE(iconFile.read(6), QByteArray::fromHex("000001000700"));
    QImage original(48, 48, QImage::Format_ARGB32); original.fill(QColor(210, 30, 40));
    const auto composed = Ucal::Manager::composeApplicationIcon(original,
        Ucal::Manager::managerIcon().pixmap(48, 48).toImage(), 48);
    QCOMPARE(composed.pixelColor(3, 3), original.pixelColor(3, 3));
    QVERIFY(composed.pixelColor(36, 36) != original.pixelColor(36, 36));
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"open";
    const auto nativeShortcut = QDir::toNativeSeparators(shortcut).toStdWString();
    launch.lpFile = nativeShortcut.c_str();
    launch.nShow = SW_HIDE;
    QVERIFY(ShellExecuteExW(&launch));
    if (launch.hProcess != nullptr) CloseHandle(launch.hProcess);
    QTRY_COMPARE_WITH_TIMEOUT(requests.count(), 1, 10000);
    QCOMPARE(requests.takeFirst().at(0).toString(), QStringLiteral("product-profile"));
#else
    QSKIP("Windows shell links require Windows.");
#endif
}

void ManagerModelTest::singleInstanceForwardsProfileByWorkspace() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workspace = directory.filePath(QStringLiteral("workspace.json"));
    Ucal::Manager::SingleInstanceController primary;
    QString error;
    QVERIFY2(primary.acquirePrimary(workspace, &error), qPrintable(error));

    Ucal::Manager::SingleInstanceController duplicate;
    QVERIFY(!duplicate.acquirePrimary(workspace, &error));
    QVERIFY(error.isEmpty());

    QSignalSpy requests(&primary,
        &Ucal::Manager::SingleInstanceController::requestReceived);
    QFuture<bool> forwarded = QtConcurrent::run([workspace]() {
        QString forwardError;
        return Ucal::Manager::SingleInstanceController::forwardToPrimary(
            workspace, QStringLiteral("application-test"), &forwardError);
    });
    QTRY_VERIFY_WITH_TIMEOUT(forwarded.isFinished(), 5000);
    QVERIFY(forwarded.result());
    QTRY_COMPARE(requests.count(), 1);
    QCOMPARE(requests.takeFirst().at(0).toString(), QStringLiteral("application-test"));

    Ucal::Manager::SingleInstanceController isolated;
    QVERIFY2(isolated.acquirePrimary(directory.filePath(QStringLiteral("other.json")), &error),
             qPrintable(error));
}

void ManagerModelTest::singleInstanceModalRequestCanRepeat() {
    QTemporaryDir directory;
    Ucal::Manager::SingleInstanceController primary;
    const QString workspace = directory.filePath(QStringLiteral("modal.json"));
    QString error;
    QVERIFY(primary.acquirePrimary(workspace, &error));
    int handled = 0;
    connect(&primary, &Ucal::Manager::SingleInstanceController::requestReceived,
            &primary, [&handled](const QString&) {
        QMessageBox modal;
        modal.setText(QStringLiteral("Already running"));
        QTimer::singleShot(30, &modal, &QDialog::accept);
        modal.exec();
        ++handled;
    });
    for (int request = 1; request <= 4; ++request) {
        auto forwarded = QtConcurrent::run([workspace]() {
            return Ucal::Manager::SingleInstanceController::forwardToPrimary(
                workspace, QStringLiteral("active-profile"));
        });
        QTRY_VERIFY_WITH_TIMEOUT(forwarded.isFinished(), 5000);
        QVERIFY(forwarded.result());
        QTRY_COMPARE(handled, request);
    }
}

void ManagerModelTest::rebuiltChannelTableHasNoVisibleStaleEditors() {
    Ucal::Manager::MainWindow window(PresentationMode::Realistic, UiLanguage::SimplifiedChinese,
                                  false, false);
    bool checked = false;
    QTimer::singleShot(0, &window, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        QTimer::singleShot(10000, dialog, &QDialog::reject);
        auto* selected = dialog->findChild<QRadioButton*>(QStringLiteral("assignmentSelected"));
        auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("applicationChannelTable"));
        QVERIFY(selected && table);
        QVERIFY(!dialog->findChild<QComboBox*>(QStringLiteral("applicationChannelCount")));
        selected->setChecked(true);
        while (table->rowCount() < 17) {
            qobject_cast<QPushButton*>(table->cellWidget(table->rowCount() - 1, 0))->click();
            qobject_cast<QComboBox*>(table->cellWidget(table->rowCount() - 2, 1))->hidePopup();
        }
        QVERIFY(!table->cellWidget(16, 0)->isEnabled());
        auto removeRow = [&](int row) {
            QTimer::singleShot(0, [] { auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                QVERIFY(box); box->button(QMessageBox::Yes)->click(); });
            qobject_cast<QPushButton*>(table->cellWidget(row, 3))->click();
        };
        while (table->rowCount() > 5) removeRow(table->rowCount() - 2);
        int visibleEditors = 0;
        for (auto* choice : table->findChildren<QComboBox*>())
            if (!choice->isHidden()) ++visibleEditors;
        QCOMPARE(visibleEditors, 4);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("CAN 1"));
        saveReviewSnapshot(dialog, QStringLiteral("selected-channels"));
        auto* choice = qobject_cast<QComboBox*>(table->cellWidget(0, 1));
        choice->setFocus();
        QTest::keyClick(choice, Qt::Key_Return);
        QVERIFY(dialog->isVisible());
        while (table->rowCount() > 1) removeRow(0);
        QVERIFY(!dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
        qobject_cast<QPushButton*>(table->cellWidget(0, 0))->click();
        qobject_cast<QComboBox*>(table->cellWidget(0, 1))->hidePopup();
        QVERIFY(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
        checked = true;
        dialog->reject();
    });
    window.showApplicationEditorForReview(1);
    QVERIFY(checked);
}

void ManagerModelTest::legacyChannelRowsRequireExplicitRemoval() {
    QTemporaryDir directory;
    const auto workspace = directory.filePath(QStringLiteral("legacy.json"));
    auto model = ManagerModel::demo();
    const auto id = QStringLiteral("pcanview-bench");
    QVERIFY(model.setApplicationChannelCount(id, 2));
    const auto remaining = model.endpoints().at(2).id;
    QVERIFY(model.setApplicationMapping(id, 2, remaining));
    model.setActiveProject(QStringLiteral("default"));
    QVERIFY(model.saveToFile(workspace));
    Ucal::Manager::MainWindow window(PresentationMode::Explanatory, UiLanguage::English,
        false, true, workspace);
    QPushButton* settings = nullptr;
    for (auto* button : window.findChildren<QPushButton*>())
        if (button->property("applicationSettings").toString() == id) settings = button;
    QVERIFY(settings);
    bool inspected = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QTimer::singleShot(10000, dialog, &QDialog::reject);
        QCOMPARE(dialog->findChild<QTabWidget*>()->currentIndex(), 1);
        auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("applicationChannelTable"));
        auto* save = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        QVERIFY(table && save);
        QCOMPARE(table->rowCount(), 3);
        QVERIFY(!save->isEnabled());
        QTimer::singleShot(0, [] { auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box); box->button(QMessageBox::Yes)->click(); });
        qobject_cast<QPushButton*>(table->cellWidget(0, 3))->click();
        QCOMPARE(table->rowCount(), 2);
        QVERIFY(save->isEnabled());
        QVERIFY(!table->cellWidget(1, 0)->isEnabled());
        inspected = true;
        save->click();
    });
    settings->click();
    QVERIFY(inspected);
    ManagerModel saved;
    QVERIFY(ManagerModel::loadFromFile(workspace, &saved));
    QCOMPARE(saved.applicationById(id)->mappings.size(), 1);
    QCOMPARE(saved.applicationById(id)->mappings.first().endpointId, remaining);
}

void ManagerModelTest::pcanDefaultsRequireAnExplicitEndpoint() {
    ManagerModel model = ManagerModel::realisticDemo();
    for (const auto& software : model.softwareInstallations()) {
        if (software.family != QStringLiteral("pcanview")) continue;
        QString id;
        QVERIFY(model.addApplication(software.id, QStringLiteral("New PCAN"), &id));
        const auto* application = model.applicationById(id);
        QCOMPARE(application->assignmentMode, AssignmentMode::Selected);
        QCOMPARE(application->channelCount, 1);
        QVERIFY(application->mappings.first().endpointId.isEmpty());
        Ucal::Manager::LaunchEnvironment environment;
        QVERIFY(!LaunchCoordinator::prepare(model, id, true, false, environment).ready);
    }
}

void ManagerModelTest::standardButtonsFollowUiLanguage() {
    struct Labels { UiLanguage language; const char* yes; const char* no; const char* cancel; };
    for (const auto& labels : {
             Labels{UiLanguage::English, "Yes", "No", "Cancel"},
             Labels{UiLanguage::SimplifiedChinese, "是", "否", "取消"},
             Labels{UiLanguage::German, "Ja", "Nein", "Abbrechen"},
             Labels{UiLanguage::Japanese, "はい", "いいえ", "キャンセル"}}) {
        Ucal::Manager::installStandardButtonTranslations(labels.language);
        QMessageBox box(QMessageBox::Question, QString(), QString(),
                       QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        QCOMPARE(box.button(QMessageBox::Yes)->text().remove(QLatin1Char('&')), QString::fromUtf8(labels.yes));
        QCOMPARE(box.button(QMessageBox::No)->text().remove(QLatin1Char('&')), QString::fromUtf8(labels.no));
        QCOMPARE(box.button(QMessageBox::Cancel)->text().remove(QLatin1Char('&')), QString::fromUtf8(labels.cancel));
    }
    Ucal::Manager::installStandardButtonTranslations(UiLanguage::English);
}

void ManagerModelTest::productLauncherUsesDirectAcceptedRoutes() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString canoeRoot = directory.filePath(QStringLiteral("CANoe"));
    const QString exec64 = QDir(canoeRoot).filePath(QStringLiteral("Exec64"));
    const QString exec32 = QDir(canoeRoot).filePath(QStringLiteral("Exec32"));
    QVERIFY(QDir().mkpath(exec64));
    QVERIFY(QDir().mkpath(exec32));
    const QString canoe = createFile(QDir(exec64).filePath(QStringLiteral("CANoe64.exe")));
    QVERIFY(!canoe.isEmpty());
    const QStringList routeFiles{
        QDir(exec32).filePath(QStringLiteral("vcndrvms.dll")),
        QDir(exec64).filePath(QStringLiteral("vcndrvms64.dll")),
        QDir(exec32).filePath(QStringLiteral("vcndrvms_origin.dll")),
        QDir(exec64).filePath(QStringLiteral("vcndrvms64_origin.dll")),
        QDir(exec32).filePath(QStringLiteral("UcalVcndrvmsDataIntercept.dll")),
        QDir(exec64).filePath(QStringLiteral("UcalVcndrvmsDataIntercept64.dll")),
        QDir(exec32).filePath(QStringLiteral("UcalVcndrvms32Discovery.dll")),
        QDir(exec64).filePath(QStringLiteral("UcalVcndrvms64Discovery.dll")),
        QDir(exec32).filePath(QStringLiteral("ucal_vcndrvms.enabled"))};
    for (const QString& file : routeFiles) QVERIFY2(!createFile(file).isEmpty(), qPrintable(file));

    Ucal::Manager::LaunchPlan canoePlan;
    canoePlan.ready = true;
    canoePlan.frontend = QStringLiteral("CANoe");
    canoePlan.applicationId = QStringLiteral("canoe-profile");
    canoePlan.softwareExecutable = canoe;
    canoePlan.integrationAdapterId = QStringLiteral("vxl-process-route");
    canoePlan.softwareVersion = QStringLiteral("11.0.42.0");
    canoePlan.backendFamilies = QStringList{QStringLiteral("MOCKAPI")};
    canoePlan.endpoints.push_back({1, QStringLiteral("endpoint-1"), QStringLiteral("adapter-1"),
                                   QStringLiteral("MOCKAPI"), QStringLiteral("serial"),
                                   QStringLiteral("CAN"), 0});
    Ucal::Manager::ProductLaunchSpec canoeSpec;
    {
        // Product prerequisites are real checks; provide a process-private
        // registration fixture instead of depending on installed Vector drivers.
        RegistryFixture fixture;
        QSettings registration(fixture.root, QSettings::Registry64Format);
        registration.setValue(QStringLiteral("SOFTWARE/VECTOR/CANDriver 1.0/Drivers/Virtual/DriverName"),
                              QStringLiteral("vcanv"));
        registration.sync();
        QVERIFY(fixture.overrideMachineView());
        canoeSpec = Ucal::Manager::ProductLauncher::prepare(
            canoePlan, directory.path(), directory.path());
        auto canoe12 = canoePlan;
        canoe12.softwareVersion = QStringLiteral("12.0.75.0");
        const auto sibling = Ucal::Manager::ProductLauncher::prepare(
            canoe12, directory.path(), directory.path());
        QVERIFY2(sibling.valid(), qPrintable(sibling.error));
    }
    QVERIFY2(canoeSpec.valid(), qPrintable(canoeSpec.error));
    QCOMPARE(canoeSpec.program, QFileInfo(canoe).absoluteFilePath());
    QCOMPARE(canoeSpec.environment.value(QStringLiteral("UCAL_PROFILE_ID")),
             QStringLiteral("canoe-profile"));
    QCOMPARE(canoeSpec.environment.value(QStringLiteral("UCAL_VCNDRVMS_CAN_ENDPOINTS")),
             QStringLiteral("endpoint-1"));
    QCOMPARE(canoeSpec.environment.value(QStringLiteral("UCAL_CANOE_HARDWARE_PRESENTATION")), QStringLiteral("1"));
    QCOMPARE(canoeSpec.environment.value(QStringLiteral("UCAL_CANOE_TIMING_TARGETS")), QStringLiteral("1"));
    QVERIFY(!canoeSpec.environment.contains(QStringLiteral("UCAL_CAN_TERMINATION")));

    const QString adaptersDirectory = QDir(directory.path()).filePath(
        QStringLiteral("runtime/x64/adapters"));
    QVERIFY(QDir().mkpath(adaptersDirectory));
    const QString bridge = createFile(QDir(directory.path()).filePath(
        QStringLiteral("runtime/x64/adapters/UcalPcanViewNativeBridge.exe")));
    const QString adapter = createFile(QDir(directory.path()).filePath(
        QStringLiteral("runtime/x64/adapters/UcalPcanViewNativeAdapter.dll")));
    QVERIFY(!createFile(QDir(adaptersDirectory).filePath(
        QStringLiteral("UcalPcanViewSupport.dll"))).isEmpty());
    const QString pcanView = createFile(directory.filePath(QStringLiteral("PcanView.exe")),
                                        QByteArray("pcan-view"));
    const QString peakApi = createFile(directory.filePath(QStringLiteral("PcanApi.dll")));
    QVERIFY(!bridge.isEmpty() && !adapter.isEmpty() && !pcanView.isEmpty() && !peakApi.isEmpty());
    const QByteArray oldPeakApi = qgetenv("UCAL_MANAGER_PEAK_API");
    qputenv("UCAL_MANAGER_PEAK_API", peakApi.toUtf8());

    Ucal::Manager::LaunchPlan pcanPlan;
    pcanPlan.ready = true;
    pcanPlan.frontend = QStringLiteral("PCANView");
    pcanPlan.applicationId = QStringLiteral("pcan-profile");
    pcanPlan.softwareExecutable = pcanView;
    pcanPlan.softwareVersion = QStringLiteral("6.0.0");
    pcanPlan.repositoryRoot = directory.path();
    pcanPlan.endpoints.push_back({1, QStringLiteral("endpoint-2"), QStringLiteral("adapter-2"),
                                  QStringLiteral("MOCKAPI"), QStringLiteral("serial"),
                                  QStringLiteral("CAN"), 0});
    const Ucal::Manager::ProductLaunchSpec pcanSpec = Ucal::Manager::ProductLauncher::prepare(
        pcanPlan, directory.path(), directory.path());
    if (oldPeakApi.isNull()) qunsetenv("UCAL_MANAGER_PEAK_API");
    else qputenv("UCAL_MANAGER_PEAK_API", oldPeakApi);
    QVERIFY2(pcanSpec.valid(), qPrintable(pcanSpec.error));
    QCOMPARE(pcanSpec.program, QFileInfo(bridge).absoluteFilePath());
    QVERIFY(pcanSpec.arguments.contains(QStringLiteral("--expected-sha256")));
    QVERIFY(pcanSpec.arguments.contains(QStringLiteral("endpoint-2")));
    QVERIFY(!pcanSpec.arguments.contains(QStringLiteral("--termination")));
    QVERIFY(pcanSpec.arguments.contains(QStringLiteral("--control-stdin")));
    QVERIFY(!pcanSpec.arguments.contains(QStringLiteral("--profile-manifest")));
    QVERIFY(!pcanSpec.arguments.contains(QStringLiteral("--bitrate")));
    QVERIFY(!pcanSpec.arguments.contains(QStringLiteral("--data-bitrate")));
    QVERIFY(!pcanSpec.arguments.contains(QStringLiteral("--can-fd")));
}

void ManagerModelTest::canoeLaunchUsesShadowWhenPresent() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString canoeRoot = directory.filePath(QStringLiteral("CANoe"));
    const QString exec64 = QDir(canoeRoot).filePath(QStringLiteral("Exec64"));
    const QString exec32 = QDir(canoeRoot).filePath(QStringLiteral("Exec32"));
    QVERIFY(QDir().mkpath(exec64));
    QVERIFY(QDir().mkpath(exec32));
    const QString canoe = createFile(QDir(exec64).filePath(QStringLiteral("CANoe64.exe")));
    QVERIFY(!canoe.isEmpty());

    // The shadow is UCAL-owned and user-writable under %LOCALAPPDATA%. Redirect
    // LOCALAPPDATA to the temporary directory so prepareCanoe resolves the shadow there.
    const QByteArray oldLocalAppData = qgetenv("LOCALAPPDATA");
    qputenv("LOCALAPPDATA", directory.path().toUtf8());
    const QString shadowRoot = QDir(directory.path()).filePath(
        QStringLiteral("UCAL/canoe-shadow/canoe-test-id"));
    const QString shadowExec64 = QDir(shadowRoot).filePath(QStringLiteral("Exec64"));
    const QString shadowExec32 = QDir(shadowRoot).filePath(QStringLiteral("Exec32"));
    QVERIFY(QDir().mkpath(shadowExec64));
    QVERIFY(QDir().mkpath(shadowExec32));
    const QString shadowCanoe = createFile(
        QDir(shadowExec64).filePath(QStringLiteral("CANoe64.exe")), QByteArray("shadow-canoe"));
    QVERIFY(!shadowCanoe.isEmpty());
    QVERIFY(!createFile(QDir(shadowRoot).filePath(
        QStringLiteral(".ucal_provider_shadow.json"))).isEmpty());
    const QStringList shadowProviderFiles{
        QDir(shadowExec32).filePath(QStringLiteral("UcalCanoeProvider.dll")),
        QDir(shadowExec64).filePath(QStringLiteral("UcalCanoeProvider64.dll")),
        QDir(shadowExec32).filePath(QStringLiteral("UcalCanoeProviderData.dll")),
        QDir(shadowExec64).filePath(QStringLiteral("UcalCanoeProviderData64.dll")),
        QDir(shadowExec32).filePath(QStringLiteral("UcalXlDiscovery.dll")),
        QDir(shadowExec64).filePath(QStringLiteral("UcalXlDiscovery64.dll")),
        QDir(shadowExec32).filePath(QStringLiteral("CANdrv32.ini")),
        QDir(shadowExec64).filePath(QStringLiteral("CANdrv64.ini"))};
    for (const QString& file : shadowProviderFiles) {
        QVERIFY2(!createFile(file).isEmpty(), qPrintable(file));
    }
    const QString shadowManifest = QDir(shadowRoot).filePath(
        QStringLiteral(".ucal_canoe_shadow_manifest.json"));
    QVERIFY(!createFile(shadowManifest, QByteArray("{\"completeRuntime\":true}")).isEmpty());
    for (const QString& relative : {QStringLiteral("vcustom.ini"), QStringLiteral("Exec64/CANoe01.dll"),
            QStringLiteral("Exec64/Vector.CANalyzer.CommonDialogs.dll"),
            QStringLiteral("Exec32/RuntimeKernel.exe"), QStringLiteral("Exec32/RTMain.dll"),
            QStringLiteral("Exec32/RTFrameworks.dll")}) {
        QVERIFY(!createFile(QDir(shadowRoot).filePath(relative)).isEmpty());
    }

    Ucal::Manager::LaunchPlan plan;
    plan.ready = true;
    plan.frontend = QStringLiteral("CANoe");
    plan.applicationId = QStringLiteral("canoe-profile");
    plan.softwareInstallationId = QStringLiteral("canoe:test-id");
    plan.softwareExecutable = canoe;
    plan.integrationAdapterId = QStringLiteral("canoe-provider");
    plan.backendFamilies = QStringList{QStringLiteral("MOCKAPI")};
    plan.endpoints.push_back({1, QStringLiteral("endpoint-1"), QStringLiteral("adapter-1"),
                              QStringLiteral("MOCKAPI"), QStringLiteral("serial"),
                              QStringLiteral("CAN"), 0});
    Ucal::Manager::SoftwareInstallation installation;
    installation.id = plan.softwareInstallationId;
    installation.family = QStringLiteral("canoe");
    installation.executablePath = canoe;
    installation.integrationAdapters = QStringList{QStringLiteral("vxl-process-route"), QStringLiteral("canoe-provider")};
    QVERIFY(!createFile(QDir(shadowExec32).filePath(QStringLiteral("vcndrvms_origin.dll"))).isEmpty());
    QVERIFY(!createFile(QDir(shadowExec64).filePath(QStringLiteral("vcndrvms64_origin.dll"))).isEmpty());
    QCOMPARE(Ucal::Manager::preferredInstalledAdapter(installation), QStringLiteral("canoe-provider"));
    const QList<QPair<QString, QString>> packagedAssets{
        {directory.filePath("frontends/canoe/x86/UcalCanoeProvider.dll"), shadowProviderFiles[2]},
        {directory.filePath("frontends/canoe/x64/UcalCanoeProvider64.dll"), shadowProviderFiles[3]},
        {directory.filePath("frontends/vxlapi/x86/UcalXlDiscovery.dll"), shadowProviderFiles[4]},
        {directory.filePath("frontends/vxlapi/x64/UcalXlDiscovery64.dll"), shadowProviderFiles[5]},
        {directory.filePath("frontends/canoe/x64/VCRUNTIME140.dll"), QDir(shadowExec64).filePath("VCRUNTIME140.dll")},
        {directory.filePath("frontends/canoe/x86/UcalVcndrvmsDataIntercept.dll"), QDir(shadowExec32).filePath("UcalVcndrvmsDataIntercept.dll")},
        {directory.filePath("frontends/canoe/x64/UcalVcndrvmsDataIntercept64.dll"), QDir(shadowExec64).filePath("UcalVcndrvmsDataIntercept64.dll")}};
    for (const auto& asset : packagedAssets) {
        QVERIFY(QDir().mkpath(QFileInfo(asset.first).absolutePath()));
        QVERIFY(!createFile(asset.first, QByteArray("packaged-") + QFileInfo(asset.first).fileName().toUtf8()).isEmpty());
    }
    const auto bytes = [](const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
    const auto originalShell32 = bytes(shadowProviderFiles[0]);
    const auto originalShell64 = bytes(shadowProviderFiles[1]);
    const auto originalSettings = bytes(QDir(shadowRoot).filePath("vcustom.ini"));
    const auto beforeData = bytes(shadowProviderFiles[2]);
    const auto compatibility = Ucal::Manager::invokeFrontend("canoe", "check_compatibility",
        {{"softwareVersion", "12.0.75.0"}, {"softwareArchitecture", "x64"},
         {"integrationAdapterId", "canoe-provider"}, {"applicationDirectory", directory.path()},
         {"plan", plan.toJson()}});
    QVERIFY(compatibility.ok());
    QVERIFY(compatibility.value.value("blockers").toArray().isEmpty());
    QCOMPARE(bytes(shadowProviderFiles[2]), beforeData);
    const auto backupRoot = QDir(shadowRoot).filePath(".ucal-runtime-backups");
    QVERIFY(!QFileInfo(backupRoot).exists());
    const Ucal::Manager::ProductLaunchSpec spec = Ucal::Manager::ProductLauncher::prepare(
        plan, directory.path(), directory.path());
    QVERIFY2(spec.valid(), qPrintable(spec.error));
    for (const auto& asset : packagedAssets) QCOMPARE(bytes(asset.second), bytes(asset.first));
    QCOMPARE(bytes(QDir(shadowExec32).filePath("UcalVcndrvms32Discovery.dll")), bytes(packagedAssets[2].first));
    QCOMPARE(bytes(QDir(shadowExec64).filePath("UcalVcndrvms64Discovery.dll")), bytes(packagedAssets[3].first));
    QCOMPARE(bytes(shadowProviderFiles[0]), originalShell32);
    QCOMPARE(bytes(shadowProviderFiles[1]), originalShell64);
    QCOMPARE(bytes(QDir(shadowRoot).filePath("vcustom.ini")), originalSettings);
    auto updates = QDir(backupRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(updates.size(), 1);
    QCOMPARE(bytes(QDir(backupRoot).filePath(updates[0] + "/Exec32/UcalCanoeProviderData.dll")), beforeData);
    QVERIFY(Ucal::Manager::ProductLauncher::prepare(plan, directory.path(), directory.path()).valid());
    QCOMPARE(QDir(backupRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot), updates);
#ifdef Q_OS_WIN
    const auto previous32 = bytes(packagedAssets[0].second);
    const auto previous64 = bytes(packagedAssets[1].second);
    QVERIFY(!createFile(packagedAssets[0].first, QByteArray("new-data32")).isEmpty());
    QVERIFY(!createFile(packagedAssets[1].first, QByteArray("new-data64")).isEmpty());
    const auto busy = CreateFileW(reinterpret_cast<LPCWSTR>(packagedAssets[1].second.utf16()), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(busy != INVALID_HANDLE_VALUE);
    const auto blocked = Ucal::Manager::ProductLauncher::prepare(plan, directory.path(), directory.path());
    CloseHandle(busy);
    QVERIFY(!blocked.valid());
    QCOMPARE(bytes(packagedAssets[0].second), previous32);
    QCOMPARE(bytes(packagedAssets[1].second), previous64);
    QVERIFY(Ucal::Manager::ProductLauncher::prepare(plan, directory.path(), directory.path()).valid());
    QCOMPARE(bytes(packagedAssets[0].second), QByteArray("new-data32"));
    QCOMPARE(bytes(packagedAssets[1].second), QByteArray("new-data64"));
#endif
    // Missing package assets must block launch without accepting cached bytes.
    QVERIFY(QFile::rename(packagedAssets[2].first, packagedAssets[2].first + ".missing"));
    QVERIFY(!Ucal::Manager::ProductLauncher::prepare(plan, directory.path(), directory.path()).valid());
    QCOMPARE(bytes(packagedAssets[2].second), QByteArray("packaged-UcalXlDiscovery.dll"));
    QVERIFY(QFile::rename(packagedAssets[2].first + ".missing", packagedAssets[2].first));
    // The vxl route still consumes the Provider forwarder selected by CANdrv.
    // An old ProviderData/shared-discovery pair must not survive its update.
    const QStringList processShells{
        QDir(shadowExec32).filePath("vcndrvms.dll"), QDir(shadowExec64).filePath("vcndrvms64.dll"),
        QDir(shadowExec32).filePath("ucal_vcndrvms.enabled")};
    for (const auto& path : processShells) QVERIFY(!createFile(path).isEmpty());
    QVERIFY(!createFile(packagedAssets[0].first, QByteArray("vxl-route-provider32")).isEmpty());
    QVERIFY(!createFile(packagedAssets[2].first, QByteArray("vxl-route-discovery32")).isEmpty());
    auto processPlan = plan;
    processPlan.integrationAdapterId = QStringLiteral("vxl-process-route");
    processPlan.softwareVersion = QStringLiteral("11.0.42.0");
    {
        RegistryFixture fixture;
        QSettings registration(fixture.root, QSettings::Registry64Format);
        registration.setValue(QStringLiteral("SOFTWARE/VECTOR/CANDriver 1.0/Drivers/Virtual/DriverName"), QStringLiteral("vcanv"));
        registration.sync();
        QVERIFY(fixture.overrideMachineView());
        const auto updated = Ucal::Manager::ProductLauncher::prepare(processPlan, directory.path(), directory.path());
        QVERIFY2(updated.valid(), qPrintable(updated.error));
    }
    QCOMPARE(bytes(shadowProviderFiles[2]), QByteArray("vxl-route-provider32"));
    QCOMPARE(bytes(shadowProviderFiles[4]), QByteArray("vxl-route-discovery32"));
    QCOMPARE(bytes(QDir(shadowExec32).filePath("UcalVcndrvms32Discovery.dll")), QByteArray("vxl-route-discovery32"));
    // Restore the provider-only fixture shape for the legacy-name checks below.
    for (const auto& path : processShells) QVERIFY(QFile::remove(path));
    QCOMPARE(spec.program, QFileInfo(shadowCanoe).absoluteFilePath());
    QVERIFY(spec.program != QFileInfo(canoe).absoluteFilePath());
    // An incomplete historical shadow cannot override an installed route.
    QVERIFY(!createFile(QDir(canoeRoot).filePath(QStringLiteral(".ucal_provider_shadow.json"))).isEmpty());
    for (const QString& file : shadowProviderFiles) {
        QString installed = file;
        installed.replace(shadowRoot, canoeRoot);
        QVERIFY(!createFile(installed).isEmpty());
    }
    QVERIFY(QFile::remove(shadowManifest));
    const auto installed = Ucal::Manager::ProductLauncher::prepare(plan, directory.path(), directory.path());
    QVERIFY2(installed.valid(), qPrintable(installed.error));
    QCOMPARE(installed.program, QFileInfo(canoe).absoluteFilePath());
    QVERIFY(!createFile(shadowManifest, QByteArray("{\"completeRuntime\":true}")).isEmpty());
    const auto complete = Ucal::Manager::ProductLauncher::prepare(plan, directory.path(), directory.path());
    QVERIFY(complete.valid());
    QCOMPARE(complete.program, QFileInfo(shadowCanoe).absoluteFilePath());
    QCOMPARE(complete.environment.value(QStringLiteral("UCAL_VECTOR_CAN_ONLY")), QStringLiteral("1"));
    const QString customization = QDir(shadowRoot).filePath(QStringLiteral("vcustom.ini"));
    QVERIFY(QFile::rename(customization, customization + QStringLiteral(".missing")));
    const auto missingSettings = Ucal::Manager::ProductLauncher::prepare(
        plan, directory.path(), directory.path());
    QVERIFY2(missingSettings.valid(), qPrintable(missingSettings.error));
    QCOMPARE(missingSettings.program, QFileInfo(canoe).absoluteFilePath());
    QVERIFY(QFile::rename(customization + QStringLiteral(".missing"), customization));
    const QList<QPair<QString, QString>> legacyAssets{
        {shadowManifest, QDir(shadowRoot).filePath(QStringLiteral(".ucal-canoe-shadow-manifest.json"))},
        {QDir(shadowRoot).filePath(QStringLiteral(".ucal_provider_shadow.json")),
         QDir(shadowRoot).filePath(QStringLiteral(".ucal-provider-shadow.json"))},
        {shadowProviderFiles[0], QDir(shadowExec32).filePath(QStringLiteral("ucal-canoe-provider.dll"))},
        {shadowProviderFiles[1], QDir(shadowExec64).filePath(QStringLiteral("ucal-canoe-provider64.dll"))}};
    for (const auto& asset : legacyAssets) QVERIFY(QFile::rename(asset.first, asset.second));
    const auto legacy = Ucal::Manager::ProductLauncher::prepare(plan, directory.path(), directory.path());
    QVERIFY2(legacy.valid(), qPrintable(legacy.error));
    QCOMPARE(legacy.program, QFileInfo(shadowCanoe).absoluteFilePath());
    QCOMPARE(Ucal::Manager::preferredInstalledAdapter(installation), QStringLiteral("canoe-provider"));
    for (const auto& asset : legacyAssets) QVERIFY(QFile::rename(asset.second, asset.first));
    const QString configuration = createFile(directory.filePath(QStringLiteral("measurement with spaces.cfg")));
    plan.configurationPath = configuration;
    const auto configured = Ucal::Manager::ProductLauncher::prepare(plan, directory.path(), directory.path());
    QVERIFY(configured.valid());
    QCOMPARE(configured.arguments, QStringList{QDir::toNativeSeparators(configuration)});
    QVERIFY(QFile::remove(configuration));
    QVERIFY(!Ucal::Manager::ProductLauncher::prepare(plan, directory.path(), directory.path()).valid());
    if (oldLocalAppData.isNull()) qunsetenv("LOCALAPPDATA");
    else qputenv("LOCALAPPDATA", oldLocalAppData);
}

void ManagerModelTest::pcanViewConcurrentLaunchesUseUniqueSlots() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString adapterDirectory = directory.filePath(QStringLiteral("runtime/x64/adapters"));
    QVERIFY(QDir().mkpath(adapterDirectory));
    QVERIFY(!createFile(QDir(adapterDirectory).filePath(
                QStringLiteral("UcalPcanViewNativeBridge.exe"))).isEmpty());
    QVERIFY(!createFile(QDir(adapterDirectory).filePath(
                QStringLiteral("UcalPcanViewNativeAdapter.dll"))).isEmpty());
    QVERIFY(!createFile(QDir(adapterDirectory).filePath(
                QStringLiteral("UcalPcanViewSupport.dll"))).isEmpty());
    const QString pcanView = createFile(directory.filePath(QStringLiteral("PcanView.exe")),
                                        QByteArray("pcan-view"));
    const QString peakApi = createFile(directory.filePath(QStringLiteral("PcanApi.dll")));
    QVERIFY(!pcanView.isEmpty() && !peakApi.isEmpty());
    const QByteArray oldPeakApi = qgetenv("UCAL_MANAGER_PEAK_API");
    qputenv("UCAL_MANAGER_PEAK_API", peakApi.toUtf8());

    Ucal::Manager::LaunchPlan plan;
    plan.ready = true;
    plan.frontend = QStringLiteral("PCANView");
    plan.applicationId = QStringLiteral("application-1");
    plan.softwareExecutable = pcanView;
    plan.softwareVersion = QStringLiteral("6.0.0");
    plan.repositoryRoot = directory.path();
    plan.endpoints.push_back({1, QStringLiteral("endpoint-1"), QStringLiteral("adapter-1"),
                              QStringLiteral("MOCKAPI"), QStringLiteral("serial"),
                              QStringLiteral("CAN"), 0});

    const Ucal::Manager::ProductLaunchSpec first = Ucal::Manager::ProductLauncher::prepare(
        plan, directory.path(), directory.path());
    QVERIFY2(first.valid(), qPrintable(first.error));
    QVERIFY(first.resourceSlot >= 1 && first.resourceSlot <= 16);

    plan.applicationId = QStringLiteral("application-5");
    const Ucal::Manager::ProductLaunchSpec second = Ucal::Manager::ProductLauncher::prepare(
        plan, directory.path(), directory.path(), UiLanguage::English,
        QSet<int>{first.resourceSlot});
    QVERIFY2(second.valid(), qPrintable(second.error));
    QVERIFY(second.resourceSlot >= 1 && second.resourceSlot <= 16);
    QVERIFY(second.resourceSlot != first.resourceSlot);

    QSet<int> allSlots;
    for (int index = 1; index <= 16; ++index) allSlots.insert(index);
    const Ucal::Manager::ProductLaunchSpec exhausted = Ucal::Manager::ProductLauncher::prepare(
        plan, directory.path(), directory.path(), UiLanguage::SimplifiedChinese, allSlots);
    QVERIFY(!exhausted.valid());
    QVERIFY(exhausted.error.startsWith(
             Ucal::Manager::profileText(UiLanguage::SimplifiedChinese,
                                QStringLiteral("launch.product.pcanSlotsExhausted"))));

    if (oldPeakApi.isNull()) qunsetenv("UCAL_MANAGER_PEAK_API");
    else qputenv("UCAL_MANAGER_PEAK_API", oldPeakApi);
}

void ManagerModelTest::launchMessagesUseSelectedLanguage() {
    for (const UiLanguage language : {UiLanguage::English, UiLanguage::SimplifiedChinese,
                                      UiLanguage::German, UiLanguage::Japanese}) {
        Ucal::Manager::LaunchEnvironment environment;
        environment.language = language;
        const Ucal::Manager::LaunchPlan missing = LaunchCoordinator::prepare(
            ManagerModel::productionDefaults(), QStringLiteral("missing-profile"),
            false, false, environment);
        QCOMPARE(missing.blockers,
                 QStringList{Ucal::Manager::uiText(
                     language, QStringLiteral("launch.blocker.profileNotFound"))});
        QVERIFY(missing.summary(language).contains(
            Ucal::Manager::uiText(language, QStringLiteral("launch.summary.blockers"))));

        Ucal::Manager::LaunchPlan unknown;
        unknown.ready = true;
        unknown.frontend = QStringLiteral("Unknown");
        const Ucal::Manager::ProductLaunchSpec spec = Ucal::Manager::ProductLauncher::prepare(
            unknown, QString(), QString(), language);
        QCOMPARE(spec.error,
                 Ucal::Manager::uiText(language,
                                    QStringLiteral("launch.product.routeUnavailable")));
    }
}

void ManagerModelTest::manualSoftwareRemovalHonorsReferences() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = createFile(directory.filePath(QStringLiteral("tool.exe")));
    ManagerModel model = ManagerModel::productionDefaults();
    const QString softwareId = model.addManualSoftware(executable);
    QVERIFY(!softwareId.isEmpty());
    QString applicationId;
    auto legacy = model.toJson();
    auto application = ManagerModel::demo().toJson().value("applications").toArray().first().toObject();
    applicationId = application.value("id").toString();
    application.insert("softwareInstallationId", softwareId);
    application.insert("name", QStringLiteral("Tool profile"));
    legacy.insert("applications", QJsonArray{application});
    legacy.insert("softwareBindings", QJsonArray{QJsonObject{{"id", softwareId}, {"source", "Manual"}, {"executablePath", executable}}});
    QVERIFY(ManagerModel::fromJson(legacy, &model));
    QCOMPARE(model.applicationNamesUsingSoftware(softwareId),
             QStringList{QStringLiteral("Tool profile")});
    QVERIFY(!model.removeManualSoftware(softwareId));
    QVERIFY(model.removeApplication(applicationId));
    QVERIFY(model.removeManualSoftware(softwareId));
    QVERIFY(model.softwareById(softwareId) == nullptr);
}

void ManagerModelTest::projectMutationsPreserveSharedApplications() {
    ManagerModel model = ManagerModel::demo();
    QVERIFY(!model.applications().isEmpty());
    const QString applicationId = model.applications().first().id;
    QString projectId;
    QVERIFY(model.addProject(QStringLiteral("Bench"), &projectId));
    QVERIFY(model.setProjectApplications(projectId, {applicationId}));
    QVERIFY(model.renameProject(projectId, QStringLiteral("Bench renamed")));
    QCOMPARE(model.applicationsForProject(projectId).size(), 1);
    QVERIFY(model.setActiveProject(projectId));
    QVERIFY(model.removeProject(projectId));
    QCOMPARE(model.activeProjectId(), QStringLiteral("default"));
    QVERIFY(model.applicationById(applicationId) != nullptr);
}

void ManagerModelTest::launchPlanUsesRawEndpointIdentity() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = createFile(directory.filePath(QStringLiteral("CANoe64.exe")));
    QVERIFY(QFileInfo(executable).isFile());
    ManagerModel model = ManagerModel::productionDefaults();
    Ucal::Manager::SoftwareInstallation canoe{
        QStringLiteral("canoe:test"), QStringLiteral("canoe"), QStringLiteral("Vector CANoe"),
        QStringLiteral("12.0.75"), QStringLiteral("x64"), executable,
        QStringLiteral("test"), QStringLiteral("Engineering route"),
        QStringLiteral("success"),
        {QStringLiteral("canoe-provider"), QStringLiteral("vxl-process-route")}, true};
    model.reconcileSoftwareInstallations({canoe});
    const QVector<Endpoint> liveEndpoints{
        {QStringLiteral("11111111-1111-4111-8111-111111111151"),
         QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"), QStringLiteral("PCAN-Basic"),
         QStringLiteral("Test vendor"), QStringLiteral("CAN interface"), QStringLiteral("TEST-SERIAL"),
         QStringLiteral("CAN"), 1, QString(), QStringLiteral("CAN / CAN FD"),
         QStringLiteral("test"), EndpointState::Online, 0x51u, true}
    };
    model.reconcileInventory(liveEndpoints);
    QString applicationId;
    QVERIFY(model.addApplication(canoe.id, QStringLiteral("CANoe test"), &applicationId));
    auto* application = model.mutableApplicationById(applicationId);
    QVERIFY(application != nullptr);
    application->integrationAdapterId = QStringLiteral("vxl-process-route");
    QVERIFY(model.setApplicationChannelCount(applicationId, liveEndpoints.size()));
    for (int index = 0; index < liveEndpoints.size(); ++index) {
        QVERIFY(model.setApplicationMapping(
            applicationId, index + 1, liveEndpoints.at(index).id));
    }

    Ucal::Manager::LaunchEnvironment environment;
    environment.applicationDirectory = directory.path();
    environment.inspectLaunchAssets = false;
    Ucal::Manager::LaunchPlan plan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, true, false, environment);
    QVERIFY2(plan.ready, qPrintable(plan.blockers.join(QStringLiteral(" | "))));
    QCOMPARE(plan.endpoints.size(), liveEndpoints.size());
    QCOMPARE(plan.endpoints.first().physicalIndex, liveEndpoints.first().physicalIndex);
    QCOMPARE(plan.endpoints.first().logicalChannel, 1);
    QCOMPARE(plan.endpoints.first().endpointId, liveEndpoints.first().id);
    application->integrationAdapterId = QStringLiteral("canoe-provider");
    const Ucal::Manager::LaunchPlan providerPlan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, true, false, environment);
    QVERIFY2(providerPlan.ready,
             qPrintable(providerPlan.blockers.join(QStringLiteral(" | "))));
    QCOMPARE(providerPlan.frontend, QStringLiteral("CANoe"));
}

void ManagerModelTest::pcanViewLaunchPlanUsesExactEndpoint() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = createFile(directory.filePath(QStringLiteral("PCANView.exe")));
    QVERIFY(QFileInfo(executable).isFile());
    ManagerModel model = ManagerModel::productionDefaults();
    Ucal::Manager::SoftwareInstallation pcanView{
        QStringLiteral("pcanview:test"), QStringLiteral("pcanview"),
        QStringLiteral("PEAK PCAN-View"), QStringLiteral("5.0.2.838"),
        QStringLiteral("x64"), executable, QStringLiteral("test"),
        QStringLiteral("Structural engineering route"), QStringLiteral("warning"),
        {QStringLiteral("pcanview5-6-structural-adapter")}, true};
    model.reconcileSoftwareInstallations({pcanView});
    Endpoint endpoint{
        QStringLiteral("11111111-1111-4111-8111-111111111151"),
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("PCAN-Basic"), QStringLiteral("Test vendor"),
        QStringLiteral("CAN interface"), QStringLiteral("Not reported"),
        QStringLiteral("CAN"), 1, QString(), QStringLiteral("CAN / CAN FD"),
        QStringLiteral("test"), EndpointState::Online, 0x51u, true};
    model.reconcileInventory({endpoint});
    QString applicationId;
    QVERIFY(model.addApplication(pcanView.id, QStringLiteral("PCAN-View test"), &applicationId));
    auto* application = model.mutableApplicationById(applicationId);
    QVERIFY(application != nullptr);
    application->integrationAdapterId = QStringLiteral("pcanview5-6-structural-adapter");
    application->assignmentMode = AssignmentMode::Selected;
    QVERIFY(model.setApplicationChannelCount(applicationId, 1));
    QVERIFY(model.setApplicationMapping(applicationId, 1, endpoint.id));

    Ucal::Manager::LaunchEnvironment environment;
    environment.applicationDirectory = directory.path();
    environment.inspectLaunchAssets = false;
    Ucal::Manager::LaunchPlan plan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, true, false, environment);
    QVERIFY2(plan.ready, qPrintable(plan.blockers.join(QStringLiteral(" | "))));
    QCOMPARE(plan.frontend, QStringLiteral("PCANView"));
    QCOMPARE(plan.endpoints.size(), 1);
    QCOMPARE(plan.endpoints.first().endpointId, endpoint.id);
    QCOMPARE(plan.endpoints.first().physicalIndex, endpoint.physicalIndex);

    application->integrationAdapterId = QStringLiteral("pcanview6-structural-adapter");
    const Ucal::Manager::LaunchPlan legacyPlan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, true, false, environment);
    QVERIFY2(legacyPlan.ready, qPrintable(legacyPlan.blockers.join(QStringLiteral(" | "))));
    application->integrationAdapterId = QStringLiteral("pcanview5-6-structural-adapter");

}

void ManagerModelTest::pcanViewLaunchPlanRejectsUnsupportedAssignments() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = createFile(directory.filePath(QStringLiteral("PCANView.exe")));
    ManagerModel model = ManagerModel::productionDefaults();
    Ucal::Manager::SoftwareInstallation pcanView{
        QStringLiteral("pcanview:test"), QStringLiteral("pcanview"),
        QStringLiteral("PEAK PCAN-View"), QStringLiteral("6.0.0.1040"),
        QStringLiteral("x64"), executable, QStringLiteral("test"),
        QStringLiteral("Structural engineering route"), QStringLiteral("warning"),
        {QStringLiteral("pcanview5-6-structural-adapter")}, true};
    model.reconcileSoftwareInstallations({pcanView});
    Endpoint first{
        QStringLiteral("11111111-1111-4111-8111-111111111151"),
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("MOCKAPI"), QStringLiteral("Example Vendor A"), QStringLiteral("Mock CAN"),
        QStringLiteral("SERIAL-A"), QStringLiteral("CAN"), 1, QString(), QStringLiteral("CAN"),
        QStringLiteral("test"), EndpointState::Online, 0u, true};
    Endpoint second{
        QStringLiteral("22222222-2222-4222-8222-222222222252"),
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
        QStringLiteral("PCAN-Basic"), QStringLiteral("PEAK"), QStringLiteral("PCAN-USB FD"),
        QStringLiteral("SERIAL-B"), QStringLiteral("CAN"), 2, QString(), QStringLiteral("CAN"),
        QStringLiteral("test"), EndpointState::Online, 0x51u, true};
    model.reconcileInventory({first, second});
    QString applicationId;
    QVERIFY(model.addApplication(pcanView.id, QStringLiteral("PCAN-View invalid"), &applicationId));
    auto* application = model.mutableApplicationById(applicationId);
    QVERIFY(application != nullptr);
    application->integrationAdapterId = QStringLiteral("pcanview5-6-structural-adapter");
    application->assignmentMode = AssignmentMode::FullAccess;
    Ucal::Manager::LaunchEnvironment environment;
    environment.inspectLaunchAssets = false;

    Ucal::Manager::LaunchPlan plan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, true, false, environment);
    QVERIFY2(plan.ready, qPrintable(plan.blockers.join(QStringLiteral(" | "))));
    QCOMPARE(plan.endpoints.size(), 1);
    QCOMPARE(plan.endpoints.first().endpointId, first.id);
    plan = Ucal::Manager::LaunchCoordinator::prepare(model, applicationId, false, false, environment);
    QVERIFY(!plan.ready);
    QVERIFY(plan.blockers.contains(Ucal::Manager::uiText(
        UiLanguage::English, QStringLiteral("launch.blocker.inventoryStale"))));
    model.reconcileInventory({});
    plan = Ucal::Manager::LaunchCoordinator::prepare(model, applicationId, true, false, environment);
    QVERIFY(!plan.ready);
    QVERIFY(plan.blockers.contains(Ucal::Manager::uiText(
        UiLanguage::English, QStringLiteral("launch.blocker.noOnlineEndpoint"))));
    model.reconcileInventory({first, second});

    application->assignmentMode = AssignmentMode::Selected;
    QVERIFY(model.setApplicationChannelCount(applicationId, 2));
    QVERIFY(model.setApplicationMapping(applicationId, 1, first.id));
    QVERIFY(model.setApplicationMapping(applicationId, 2, second.id));
    plan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, true, false, environment);
    QVERIFY(!plan.ready);
    QVERIFY(plan.blockers.contains(Ucal::Manager::profileText(
        UiLanguage::English, QStringLiteral("launch.blocker.pcanOneEndpoint"))));
    QVERIFY(plan.blockers.contains(Ucal::Manager::profileText(
        UiLanguage::English, QStringLiteral("launch.blocker.pcanResolution"))));
}

void ManagerModelTest::launchPlanRejectsNonUuidIdentity() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = createFile(directory.filePath(QStringLiteral("CANoe64.exe")));
    ManagerModel model = ManagerModel::productionDefaults();
    Ucal::Manager::SoftwareInstallation canoe{
        QStringLiteral("canoe:test"), QStringLiteral("canoe"), QStringLiteral("Vector CANoe"),
        QStringLiteral("12.0.75"), QStringLiteral("x64"), executable,
        QStringLiteral("test"), QStringLiteral("Engineering route"),
        QStringLiteral("warning"), {QStringLiteral("vxl-process-route")}, true};
    model.reconcileSoftwareInstallations({canoe});
    Endpoint endpoint{
        QStringLiteral("endpoint:not-a-uuid"), QStringLiteral("adapter:not-a-uuid"),
        QStringLiteral("PCAN-Basic"), QStringLiteral("PEAK"), QStringLiteral("PCAN-USB FD"),
        QStringLiteral("TEST-SERIAL"), QStringLiteral("CAN"), 1, QString(),
        QStringLiteral("CAN / CAN FD"), QStringLiteral("test"), EndpointState::Online,
        0x51u, true};
    model.reconcileInventory({endpoint});
    QString applicationId;
    QVERIFY(model.addApplication(canoe.id, QStringLiteral("CANoe test"), &applicationId));
    auto* application = model.mutableApplicationById(applicationId);
    QVERIFY(application != nullptr);
    application->integrationAdapterId = QStringLiteral("vxl-process-route");
    QVERIFY(model.setApplicationChannelCount(applicationId, 1));
    QVERIFY(model.setApplicationMapping(applicationId, 1, endpoint.id));
    Ucal::Manager::LaunchEnvironment environment;
    environment.inspectLaunchAssets = false;
    const Ucal::Manager::LaunchPlan plan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, true, false, environment);
    QVERIFY(!plan.ready);
    QVERIFY(plan.blockers.contains(
        QStringLiteral("Logical channel 1 lacks complete stable endpoint identity.")));
}

void ManagerModelTest::canoeLaunchPlanDoesNotRequireSerial() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = createFile(directory.filePath(QStringLiteral("CANoe64.exe")));
    ManagerModel model = ManagerModel::productionDefaults();
    Ucal::Manager::SoftwareInstallation canoe{
        QStringLiteral("canoe:test"), QStringLiteral("canoe"), QStringLiteral("Vector CANoe"),
        QStringLiteral("12.0.75"), QStringLiteral("x64"), executable,
        QStringLiteral("test"), QStringLiteral("Engineering route"),
        QStringLiteral("warning"), {QStringLiteral("vxl-process-route")}, true};
    model.reconcileSoftwareInstallations({canoe});
    // A "Not reported" serial previously tripped the serial-selector blocker on the
    // CANoe route. Endpoint UUIDs already identify channels, so it must no longer block.
    Endpoint endpoint{
        QStringLiteral("11111111-1111-4111-8111-111111111151"),
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("PCAN-Basic"), QStringLiteral("PEAK"), QStringLiteral("PCAN-USB FD"),
        QStringLiteral("Not reported"), QStringLiteral("CAN"), 1, QString(),
        QStringLiteral("CAN / CAN FD"), QStringLiteral("test"), EndpointState::Online,
        0x51u, true};
    model.reconcileInventory({endpoint});
    QString applicationId;
    QVERIFY(model.addApplication(canoe.id, QStringLiteral("CANoe test"), &applicationId));
    auto* application = model.mutableApplicationById(applicationId);
    QVERIFY(application != nullptr);
    application->integrationAdapterId = QStringLiteral("vxl-process-route");
    QVERIFY(model.setApplicationChannelCount(applicationId, 1));
    QVERIFY(model.setApplicationMapping(applicationId, 1, endpoint.id));
    Ucal::Manager::LaunchEnvironment environment;
    environment.inspectLaunchAssets = false;
    const Ucal::Manager::LaunchPlan plan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, true, false, environment);
    QVERIFY2(plan.ready, qPrintable(plan.blockers.join(QStringLiteral(" | "))));
}

void ManagerModelTest::launchPlanFailsClosedOnStaleInventory() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = createFile(directory.filePath(QStringLiteral("CANoe64.exe")));
    ManagerModel model = ManagerModel::productionDefaults();
    Ucal::Manager::SoftwareInstallation canoe{
        QStringLiteral("canoe:test"), QStringLiteral("canoe"), QStringLiteral("Vector CANoe"),
        QStringLiteral("12.0.75"), QStringLiteral("x64"), executable,
        QStringLiteral("test"), QStringLiteral("Engineering route"),
        QStringLiteral("warning"), {QStringLiteral("vxl-process-route")}, true};
    model.reconcileSoftwareInstallations({canoe});
    QString applicationId;
    QVERIFY(model.addApplication(canoe.id, QStringLiteral("CANoe test"), &applicationId));
    auto* application = model.mutableApplicationById(applicationId);
    QVERIFY(application != nullptr);
    application->integrationAdapterId = QStringLiteral("vxl-process-route");

    const Ucal::Manager::LaunchPlan ucalPlan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, false, false, {});
    QVERIFY(!ucalPlan.ready);
    QVERIFY(ucalPlan.blockers.contains(Ucal::Manager::uiText(
        UiLanguage::English, QStringLiteral("launch.blocker.inventoryStale"))));

    const Ucal::Manager::LaunchPlan nativePlan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, false, true, {});
    QVERIFY2(nativePlan.ready, qPrintable(nativePlan.blockers.join(QStringLiteral(" | "))));
    QVERIFY(nativePlan.endpoints.isEmpty());
}

void ManagerModelTest::fullAccessLaunchUsesStableCatalogOrder() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = createFile(directory.filePath(QStringLiteral("CANoe64.exe")));
    ManagerModel model = ManagerModel::productionDefaults();
    Ucal::Manager::SoftwareInstallation canoe{
        QStringLiteral("canoe:test"), QStringLiteral("canoe"), QStringLiteral("Vector CANoe"),
        QStringLiteral("12.0.75"), QStringLiteral("x64"), executable,
        QStringLiteral("test"), QStringLiteral("Engineering route"),
        QStringLiteral("warning"), {QStringLiteral("vxl-process-route")}, true};
    model.reconcileSoftwareInstallations({canoe});
    Endpoint adapterZ{
        QStringLiteral("22222222-2222-4222-8222-222222222222"),
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"), QStringLiteral("MOCKAPI"),
        QStringLiteral("Example Vendor A"), QStringLiteral("Mock CAN"), QStringLiteral("SERIAL-Z"),
        QStringLiteral("CAN"), 1, QString(), QStringLiteral("CAN"), QStringLiteral("test"),
        EndpointState::Online, 0u, true};
    Endpoint adapterA{
        QStringLiteral("33333333-3333-4333-8333-333333333333"),
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"), QStringLiteral("Vector XL"),
        QStringLiteral("Vector"), QStringLiteral("Virtual CAN"), QStringLiteral("SERIAL-A"),
        QStringLiteral("CAN"), 1, QString(), QStringLiteral("CAN"), QStringLiteral("test"),
        EndpointState::Online, 0u, true};
    model.reconcileInventory({adapterZ, adapterA});
    QString applicationId;
    QVERIFY(model.addApplication(canoe.id, QStringLiteral("CANoe all"), &applicationId));
    auto* application = model.mutableApplicationById(applicationId);
    QVERIFY(application != nullptr);
    application->integrationAdapterId = QStringLiteral("vxl-process-route");
    application->assignmentMode = AssignmentMode::FullAccess;
    Ucal::Manager::LaunchEnvironment environment;
    environment.inspectLaunchAssets = false;
    const Ucal::Manager::LaunchPlan plan = Ucal::Manager::LaunchCoordinator::prepare(
        model, applicationId, true, false, environment);
    QVERIFY2(plan.ready, qPrintable(plan.blockers.join(QStringLiteral(" | "))));
    QCOMPARE(plan.endpoints.size(), 2);
    QCOMPARE(plan.endpoints.at(0).adapterId,
             QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"));
    QCOMPARE(plan.endpoints.at(1).adapterId,
             QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"));
    QCOMPARE(plan.endpoints.at(0).logicalChannel, 1);
    QCOMPARE(plan.endpoints.at(1).logicalChannel, 2);
    QVector<Endpoint> many;
    for (int i=0;i<40;++i) {
        auto endpoint=adapterA;
        endpoint.id=QStringLiteral("00000000-0000-4000-8000-%1").arg(i+1,12,16,QLatin1Char('0'));
        endpoint.physicalIndex=quint32(i); endpoint.physicalIndexValid=true; endpoint.physicalChannel=i+1;
        many << endpoint;
    }
    model.reconcileInventory(many);
    QCOMPARE(model.fullAccessMappings().size(),40);
    QCOMPARE(model.effectiveMappingsForApplication(applicationId).size(),16);
    model.mutableApplicationById(applicationId)->channelCount=32; // legacy selected count has no authority in automatic mode
    const auto bounded=LaunchCoordinator::prepare(model,applicationId,true,false,environment);
    QVERIFY2(bounded.ready,qPrintable(bounded.blockers.join(QStringLiteral(" | "))));
    QCOMPARE(bounded.endpoints.size(),16);
    for(int i=0;i<16;++i) QCOMPARE(bounded.endpoints[i].endpointId,many[i].id);
    QCOMPARE(model.fullAccessMappings().size(),40);
    auto pcan = canoe;
    pcan.id = QStringLiteral("pcanview:test"); pcan.family = QStringLiteral("pcanview");
    pcan.displayName = QStringLiteral("PEAK PCAN-View"); pcan.version = QStringLiteral("6.0.0.1040");
    pcan.integrationAdapters = QStringList{QStringLiteral("pcanview5-6-structural-adapter")};
    model.reconcileSoftwareInstallations({canoe, pcan});
    QString pcanId;
    QVERIFY(model.addApplication(pcan.id, QStringLiteral("PCAN all"), &pcanId));
    QVERIFY(model.setApplicationMapping(pcanId, 1, many.last().id));
    QVERIFY(model.setApplicationAssignmentMode(pcanId, AssignmentMode::FullAccess));
    QCOMPARE(model.effectiveMappingsForApplication(pcanId).size(), 1);
    const auto single = LaunchCoordinator::prepare(model, pcanId, true, false, environment);
    QVERIFY2(single.ready, qPrintable(single.blockers.join(QStringLiteral(" | "))));
    QCOMPARE(single.endpoints.size(), 1);
    QCOMPARE(single.endpoints.first().endpointId, bounded.endpoints.first().endpointId);
    QCOMPARE(single.endpoints.first().endpointId, model.effectiveMappingsForApplication(pcanId).first().endpointId);
    std::reverse(many.begin(), many.end());
    model.reconcileInventory(many);
    const auto reordered = LaunchCoordinator::prepare(model, pcanId, true, false, environment);
    QVERIFY(reordered.ready);
    QCOMPARE(reordered.endpoints.first().endpointId, single.endpoints.first().endpointId);
    // Automatic resolution never overwrites a user's saved explicit binding.
    QVERIFY(model.setApplicationAssignmentMode(pcanId, AssignmentMode::Selected));
    QCOMPARE(model.effectiveMappingsForApplication(pcanId).first().endpointId, many.first().id);
}

void ManagerModelTest::pcanFullAccessEditorPersistsMode() {
    QTemporaryDir directory;
    const auto workspace = directory.filePath(QStringLiteral("pcan-mode.json"));
    auto model = ManagerModel::productionDefaults();
    Ucal::Manager::SoftwareInstallation software{
        QStringLiteral("pcanview:editor"), QStringLiteral("pcanview"), QStringLiteral("PEAK PCAN-View"),
        QStringLiteral("6.0.0.1040"), QStringLiteral("x64"),
        createFile(directory.filePath(QStringLiteral("PCANView.exe"))), QStringLiteral("test"),
        QStringLiteral("Engineering comparator"), QStringLiteral("warning"),
        {QStringLiteral("pcanview5-6-structural-adapter")}, true};
    model.reconcileSoftwareInstallations({software});
    QString id;
    QVERIFY(model.addApplication(software.id, QStringLiteral("PCAN mode"), &id));
    model.reconcileInventory(ManagerModel::demo().endpoints());
    const QString endpoint = model.endpoints().first().id;
    QVERIFY(model.setApplicationMapping(id, 1, endpoint));
    QVERIFY(model.saveToFile(workspace));
    Ucal::Manager::MainWindow window(PresentationMode::Explanatory, UiLanguage::English, false, true, workspace);
    window.show();
    bool checked = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog); QTimer::singleShot(5000, dialog, &QDialog::reject);
        auto* full = dialog->findChild<QRadioButton*>(QStringLiteral("assignmentFullAccess"));
        auto* selected = dialog->findChild<QRadioButton*>(QStringLiteral("assignmentSelected"));
        auto* save = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        QVERIFY(full && selected && full->isEnabled());
        QVERIFY(selected->isChecked());
        full->click(); QVERIFY(full->isChecked()); QVERIFY(save->isEnabled());
        checked = true; save->click();
    });
    window.showApplicationEditorForReview(1); QVERIFY(checked);
    ManagerModel saved; QVERIFY(ManagerModel::loadFromFile(workspace, &saved));
    QCOMPARE(saved.applicationById(id)->assignmentMode, AssignmentMode::FullAccess);
    QCOMPARE(saved.applicationById(id)->mappings.first().endpointId, endpoint);
    checked = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog); QTimer::singleShot(5000, dialog, &QDialog::reject);
        auto* full = dialog->findChild<QRadioButton*>(QStringLiteral("assignmentFullAccess"));
        auto* selected = dialog->findChild<QRadioButton*>(QStringLiteral("assignmentSelected"));
        auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("applicationChannelTable"));
        QVERIFY(full && selected && table && full->isEnabled());
        QVERIFY(full->isChecked());
        // An offline persisted reference is not restored as an online candidate.
        QCOMPARE(table->rowCount(), 0);
        selected->click(); QCOMPARE(table->rowCount(), 2); // one mapping plus the add-row footer
        auto* choice = qobject_cast<QComboBox*>(table->cellWidget(0, 1));
        QVERIFY(choice && choice->isEnabled()); QCOMPARE(choice->currentData().toString(), endpoint);
        checked = true; dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    window.showApplicationEditorForReview(1); QVERIFY(checked);
    QVERIFY(ManagerModel::loadFromFile(workspace, &saved));
    QCOMPARE(saved.applicationById(id)->assignmentMode, AssignmentMode::Selected);
    QCOMPARE(saved.applicationById(id)->mappings.first().endpointId, endpoint);
}

void ManagerModelTest::reviewNavigationAndSoftwareUsage() {
    QTemporaryDir directory;
    Ucal::Manager::MainWindow empty(PresentationMode::Production, UiLanguage::English,
        false, false, directory.filePath(QStringLiteral("empty.json")));
    empty.show();
    QApplication::processEvents();
    QVERIFY(empty.findChild<QWidget*>(QStringLiteral("applicationQuickStart")));
    int lastY = -1;
    for (const auto& id : {"navApplications", "navSoftware", "navBackends", "navHardware", "navSettings"}) {
        QPushButton* found = nullptr;
        for (auto* button : empty.findChildren<QPushButton*>())
            if (button->property("automationId").toString() == QLatin1String(id)) found = button;
        QVERIFY(found);
        QVERIFY(!found->icon().isNull());
        QVERIFY(found->y() > lastY);
        lastY = found->y();
    }
    for (auto* button : empty.findChildren<QPushButton*>())
        if (button->isVisible()) QVERIFY(button->text() != QStringLiteral("Rescan"));
    empty.hide();

    Ucal::Manager::MainWindow window(PresentationMode::Explanatory, UiLanguage::English,
        false, false, directory.filePath(QStringLiteral("demo.json")));
    window.selectPage(2);
    window.show();
    QPushButton* used = nullptr;
    bool checkedEmpty = false;
    for (auto* button : window.findChildren<QPushButton*>()) {
        if (button->property("softwareApplications").toString().isEmpty()) continue;
        if (button->isEnabled()) used = button;
        else checkedEmpty = true;
    }
    QVERIFY(used);
    QVERIFY(checkedEmpty);
    bool inspected = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QTimer::singleShot(10000, dialog, &QDialog::reject);
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("softwareApplicationList"));
        QVERIFY(list && list->count() > 0);
        saveReviewSnapshot(dialog, QStringLiteral("software-applications"));
        inspected = true;
        dialog->reject();
    });
    used->click();
    QVERIFY(inspected);
    window.selectPage(3);
    QPushButton* details = nullptr;
    for (auto* button : window.findChildren<QPushButton*>())
        if (button->property("backendId").toString() == QStringLiteral("mockapi")) details = button;
    QVERIFY(details);
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        saveReviewSnapshot(dialog, QStringLiteral("driver-details"));
        dialog->reject();
    });
    details->click();
}

void ManagerModelTest::mainWindowNavigatesEveryPage() {
    Ucal::Manager::MainWindow window(PresentationMode::Realistic, UiLanguage::English,
                                  false, false);
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("mainPages"));
    QVERIFY(pages != nullptr);
    QCOMPARE(pages->count(), 5);
    for (int index = 0; index < pages->count(); ++index) {
        window.selectPage(index);
        QCOMPARE(pages->currentIndex(), index);
        QVERIFY(pages->currentWidget() != nullptr);
    }
    const auto startAllButtons = window.findChildren<QToolButton*>();
    const auto startAll = std::find_if(
        startAllButtons.cbegin(), startAllButtons.cend(), [](const QToolButton* button) {
            return button->property("ucalAction").toString() == QStringLiteral("startAll");
        });
    QVERIFY(startAll != startAllButtons.cend());
    QVERIFY((*startAll)->isEnabled());

    Ucal::Manager::MainWindow production(PresentationMode::Production, UiLanguage::English,
                                      false, false);
    const auto productionButtons = production.findChildren<QToolButton*>();
    const auto productionStartAll = std::find_if(
        productionButtons.cbegin(), productionButtons.cend(),
        [](const QToolButton* button) {
            return button->property("ucalAction").toString() == QStringLiteral("startAll");
        });
    QVERIFY(productionStartAll != productionButtons.cend());
    (*productionStartAll)->click();
    auto* status = production.findChild<QLabel*>(QStringLiteral("statusSummary"));
    QVERIFY(status != nullptr);
    QCOMPARE(production.statusBar()->currentMessage(),
             QStringLiteral("The current project has no application profiles."));
    QVERIFY(status->text().contains(QStringLiteral("inventory not current")));
}

void ManagerModelTest::languageChangePreservesWindowAndProcesses() {
    QStandardPaths::setTestModeEnabled(true);
    const UiLanguage original = Ucal::Manager::loadUiLanguagePreference();
    QVERIFY(Ucal::Manager::saveUiLanguagePreference(UiLanguage::English));

    Ucal::Manager::MainWindow window(PresentationMode::Realistic, UiLanguage::English,
                                  false, false);
    window.selectPage(4);
    auto* language = window.findChild<QComboBox*>(QStringLiteral("uiLanguagePreference"));
    QVERIFY(language != nullptr);
    QPointer<QComboBox> oldLanguage(language);

    auto* ownedProcess = new QProcess(&window);
    QPointer<QProcess> guardedProcess(ownedProcess);
    ownedProcess->start(QStringLiteral("cmd.exe"),
                        {QStringLiteral("/d"), QStringLiteral("/c"),
                         QStringLiteral("ping -n 30 127.0.0.1 >NUL")});
    QVERIFY2(ownedProcess->waitForStarted(3000), qPrintable(ownedProcess->errorString()));

    const int chinese = language->findData(
        Ucal::Manager::uiLanguageKey(UiLanguage::SimplifiedChinese));
    QVERIFY(chinese >= 0);
    language->setCurrentIndex(chinese);
    QTRY_VERIFY_WITH_TIMEOUT(oldLanguage.isNull(), 3000);

    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("mainPages"));
    QVERIFY(pages != nullptr);
    QCOMPARE(pages->currentIndex(), 4);
    auto* replacement = window.findChild<QComboBox*>(
        QStringLiteral("uiLanguagePreference"));
    QVERIFY(replacement != nullptr);
    QCOMPARE(replacement->currentData().toString(),
             Ucal::Manager::uiLanguageKey(UiLanguage::SimplifiedChinese));
    QVERIFY(!guardedProcess.isNull());
    QVERIFY(guardedProcess->state() != QProcess::NotRunning);

    guardedProcess->kill();
    QVERIFY(guardedProcess->waitForFinished(3000));
    QVERIFY(Ucal::Manager::saveUiLanguagePreference(original));
}

void ManagerModelTest::startAllDispatchesEveryProjectProfile() {
    Ucal::Manager::MainWindow window(PresentationMode::Realistic, UiLanguage::English,
                                  false, false);
    QSignalSpy dispatched(&window, &Ucal::Manager::MainWindow::profileLaunchRequested);
    const ManagerModel model = ManagerModel::realisticDemo();
    const QVector<Ucal::Manager::ApplicationProfile> expected =
        model.applicationsForProject(model.activeProjectId());
    QVERIFY(expected.size() > 1);

    auto* startAll = [&window]() -> QToolButton* {
        for (QToolButton* button : window.findChildren<QToolButton*>()) {
            if (button->property("ucalAction").toString() == QStringLiteral("startAll")) {
                return button;
            }
        }
        return nullptr;
    }();
    QVERIFY(startAll != nullptr);

    QTimer closer;
    connect(&closer, &QTimer::timeout, &window, []() {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            message->accept();
        }
    });
    closer.start(10);
    startAll->click();
    closer.stop();

    QCOMPARE(dispatched.count(), expected.size());
    for (int index = 0; index < expected.size(); ++index) {
        QCOMPARE(dispatched.at(index).at(0).toString(), expected.at(index).id);
    }
}

void ManagerModelTest::productStateLabelsRemainTruthful() {
    for (const auto language : {UiLanguage::SimplifiedChinese, UiLanguage::English, UiLanguage::German, UiLanguage::Japanese}) {
        Ucal::Manager::MainWindow compact(PresentationMode::Production, language,
                                          false, false);
        compact.show();
        QCoreApplication::processEvents();
        const auto* wordmark = compact.findChild<QLabel*>(QStringLiteral("brand"));
        QVERIFY(wordmark != nullptr);
        QCOMPARE(wordmark->text(), QStringLiteral("UCAL Manager"));
        const auto* version = compact.findChild<QLabel*>(QStringLiteral("sidebarVersionLabel"));
        QVERIFY(version != nullptr);
        QCOMPARE(version->text(), QStringLiteral("UCAL Manager 0.9.0"));
        QVERIFY(!version->wordWrap());
        QVERIFY(version->fontMetrics().horizontalAdvance(version->text()) <= version->width());
        QVERIFY(wordmark->fontMetrics().horizontalAdvance(wordmark->text()) <= wordmark->width());
        compact.hide();
    }
    Ucal::Manager::MainWindow production(PresentationMode::Production, UiLanguage::English,
                                      false, false);
    const auto* logo = production.findChild<QLabel*>(QStringLiteral("brandLogo"));
    QVERIFY(logo != nullptr);
    QVERIFY(!logo->pixmap()->isNull());
    QVERIFY(production.findChild<QLabel*>(QStringLiteral("brandAccent")) == nullptr);
    const auto* fullName = production.findChild<QLabel*>(QStringLiteral("aboutFullName"));
    QVERIFY(fullName != nullptr);
    QCOMPARE(fullName->text(), QStringLiteral("Unified CAN Abstraction Layer"));
    production.selectPage(3);
    QStringList backendTexts;
    for (const QLabel* label : production.findChildren<QLabel*>()) {
        backendTexts.push_back(label->text());
    }
    QVERIFY(!backendTexts.contains(QStringLiteral("Ready")));
    QVERIFY(!backendTexts.contains(QStringLiteral("Loaded")));
    if (!Ucal::Manager::scanBackendDirectory(Ucal::Manager::backendDirectory(QCoreApplication::applicationDirPath())).packages.isEmpty())
        QVERIFY(backendTexts.contains(QStringLiteral("Not checked")));

    Ucal::Manager::MainWindow realistic(PresentationMode::Realistic, UiLanguage::English,
                                     false, false);
    realistic.show();
    bool inspected = false;
    QTimer::singleShot(50, &realistic, [&inspected]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        QTimer::singleShot(0, dialog, &QDialog::reject);
        QStringList texts;
        for (const QLabel* label : dialog->findChildren<QLabel*>()) {
            texts.push_back(label->text());
        }
        QVERIFY(!texts.contains(QStringLiteral("UCAL_CANoe_MOCKAPI.ini · Loaded")));
        QVERIFY(!texts.contains(QStringLiteral("UCAL Provider 0.2 · x64 · Configured")));
        QVERIFY(!texts.contains(QStringLiteral("Technical details")));
        QVERIFY(!dialog->findChild<QComboBox*>(QStringLiteral("integrationAdapterCombo")));
        auto* tabs = dialog->findChild<QTabWidget*>();
        QVERIFY(tabs);
        QVERIFY(tabs->widget(0)->findChildren<QLineEdit*>().isEmpty());
        QVERIFY(tabs->widget(2)->findChild<QLineEdit*>(QStringLiteral("applicationConfigurationPath")));
        QVERIFY(tabs->widget(2)->findChild<QLineEdit*>(QStringLiteral("applicationArguments")));
        inspected = true;
        dialog->reject();
    });
    realistic.showApplicationEditorForReview(2);
    QVERIFY(inspected);
}

void ManagerModelTest::reviewDialogsExposeEveryWorkflowSurface() {
    Ucal::Manager::MainWindow window(PresentationMode::Realistic, UiLanguage::English,
                                  false, false);
    window.show();

    for (int tabIndex = 0; tabIndex < 3; ++tabIndex) {
        bool observed = false;
        QTimer::singleShot(50, &window, [&observed, tabIndex]() {
            QWidget* modal = QApplication::activeModalWidget();
            QVERIFY(modal != nullptr);
            auto* tabs = modal->findChild<QTabWidget*>();
            QVERIFY(tabs != nullptr);
            QCOMPARE(tabs->currentIndex(), tabIndex);
            if (tabIndex == 1) {
                auto* full = modal->findChild<QRadioButton*>(
                    QStringLiteral("assignmentFullAccess"));
                auto* selected = modal->findChild<QRadioButton*>(
                    QStringLiteral("assignmentSelected"));
                auto* table = modal->findChild<QTableWidget*>(
                    QStringLiteral("applicationChannelTable"));
                QVERIFY(full != nullptr);
                QVERIFY(selected != nullptr);
                QVERIFY(table != nullptr);
                selected->setChecked(true);
                QVERIFY(table->rowCount() > 0);
                auto* selectedEndpoint =
                    qobject_cast<QComboBox*>(table->cellWidget(0, 1));
                QVERIFY(selectedEndpoint != nullptr);
                QVERIFY(selectedEndpoint->isEnabled());
                full->setChecked(true);
                QVERIFY(table->isEnabled());
                QVERIFY(table->rowCount() > 0);
                auto* endpoint = qobject_cast<QComboBox*>(table->cellWidget(0, 1));
                QVERIFY(endpoint != nullptr);
                QVERIFY(!endpoint->isEnabled());
                QVERIFY(!endpoint->currentData().toString().isEmpty());
            }
            observed = true;
            modal->close();
        });
        window.showApplicationEditorForReview(tabIndex);
        QVERIFY(observed);
    }

    const QStringList dialogs = {
        QStringLiteral("project"),
        QStringLiteral("hardware-assignment")};
    for (const QString& dialog : dialogs) {
        bool observed = false;
        QTimer::singleShot(50, &window, [&observed]() {
            QWidget* modal = QApplication::activeModalWidget();
            QVERIFY(modal != nullptr);
            QVERIFY(!modal->windowTitle().isEmpty());
            observed = true;
            modal->close();
        });
        window.showDialogForReview(dialog);
        QVERIFY(observed);
    }
}

void ManagerModelTest::keyboardControlsAreAccessibleAndOperable() {
    Ucal::Manager::MainWindow window(PresentationMode::Realistic, UiLanguage::English,
                                  false, false);
    window.show();
    QApplication::processEvents();
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("mainPages"));
    auto* projectSelector = window.findChild<QComboBox*>(QStringLiteral("projectSelector"));
    QVERIFY(pages != nullptr);
    QVERIFY(projectSelector != nullptr);
    QVERIFY(projectSelector->focusPolicy() != Qt::NoFocus);
    auto* projectAccessibility = QAccessible::queryAccessibleInterface(projectSelector);
    QVERIFY(projectAccessibility != nullptr);
    QVERIFY(!projectAccessibility->text(QAccessible::Name).trimmed().isEmpty());

    const QStringList automationIds = {
        QStringLiteral("navApplications"), QStringLiteral("navHardware"),
        QStringLiteral("navSoftware"), QStringLiteral("navBackends"),
        QStringLiteral("navSettings")};
    const auto buttons = window.findChildren<QAbstractButton*>();
    for (int index = 0; index < automationIds.size(); ++index) {
        QAbstractButton* target = nullptr;
        for (QAbstractButton* button : buttons) {
            if (button->property("automationId").toString() == automationIds.at(index)) {
                target = button;
                break;
            }
        }
        QVERIFY2(target != nullptr, qPrintable(automationIds.at(index)));
        QVERIFY(target->focusPolicy() != Qt::NoFocus);
        auto* accessibility = QAccessible::queryAccessibleInterface(target);
        QVERIFY(accessibility != nullptr);
        QVERIFY(!accessibility->text(QAccessible::Name).trimmed().isEmpty());
        target->setFocus(Qt::TabFocusReason);
        QCOMPARE(QApplication::focusWidget(), target);
        QTest::keyClick(target, Qt::Key_Space);
        QCOMPARE(pages->currentIndex(), index);
    }
}

void ManagerModelTest::applicationWorkflowMutatesThroughWidgets() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workspace = directory.filePath(QStringLiteral("applications.json"));
    Ucal::Manager::MainWindow window(PresentationMode::Explanatory, UiLanguage::English,
                                  false, false, workspace);
    window.show();

    bool editorHandled = false;
    QTimer::singleShot(50, &window, [&]() {
        auto* editor = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(editor != nullptr);
        auto* name = editor->findChild<QLineEdit*>(QStringLiteral("applicationNameEdit"));
        auto* selected = editor->findChild<QRadioButton*>(QStringLiteral("assignmentSelected"));
        auto* table = editor->findChild<QTableWidget*>(QStringLiteral("applicationChannelTable"));
        auto* buttons = editor->findChild<QDialogButtonBox*>();
        QVERIFY(name && selected && table && buttons);
        // Native window activation can arrive after the first modal timer.
        // Keep failures bounded instead of leaving this editor's exec() hung.
        QTimer::singleShot(7000, editor, &QDialog::reject);
        QTRY_COMPARE(QApplication::focusWidget(), name);
        QCOMPARE(name->selectedText(), name->text());
        name->setText(QStringLiteral("Widget-configured profile"));
        selected->setChecked(true);
        qobject_cast<QPushButton*>(table->cellWidget(table->rowCount() - 1, 0))->click();
        qobject_cast<QComboBox*>(table->cellWidget(1, 1))->hidePopup();
        QCOMPARE(table->rowCount(), 3);
        for (int row = 0; row < 2; ++row) {
            auto* choice = qobject_cast<QComboBox*>(table->cellWidget(row, 1));
            QVERIFY(choice && choice->count() > row + 1);
            choice->setCurrentIndex(row + 1);
        }
        editorHandled = true;
        buttons->button(QDialogButtonBox::Save)->click();
    });
    window.showDialogForReview(QStringLiteral("add-application"));
    QPushButton* create = nullptr;
    for (auto* button : window.findChildren<QPushButton*>())
        if (!button->property("softwareId").toString().isEmpty()) { create = button; break; }
    QVERIFY(create);
    create->click();
    QVERIFY(editorHandled);

    ManagerModel saved;
    QString error;
    QVERIFY2(ManagerModel::loadFromFile(workspace, &saved, &error), qPrintable(error));
    const auto created = std::find_if(
        saved.applications().cbegin(), saved.applications().cend(), [](const auto& application) {
            return application.name == QStringLiteral("Widget-configured profile");
        });
    QVERIFY(created != saved.applications().cend());
    const QString createdId = created->id;
    const AssignmentMode createdAssignmentMode = created->assignmentMode;
    const int createdChannelCount = created->channelCount;
    const int createdMappingCount = created->mappings.size();
    QCOMPARE(created->assignmentMode, AssignmentMode::Selected);
    QCOMPARE(created->channelCount, 2);
    QCOMPARE(created->mappings.size(), 2);
    QVERIFY(!created->mappings.at(0).endpointId.isEmpty());
    QVERIFY(!created->mappings.at(1).endpointId.isEmpty());

    QStringList idsBeforeDuplicate;
    for (const auto& application : saved.applications()) idsBeforeDuplicate.push_back(application.id);
    auto* duplicate = window.findChild<QAction*>(QStringLiteral("duplicateApplication:") + createdId);
    QVERIFY(duplicate != nullptr);
    duplicate->trigger();
    QApplication::processEvents();
    QVERIFY2(ManagerModel::loadFromFile(workspace, &saved, &error), qPrintable(error));
    QCOMPARE(saved.applications().size(), idsBeforeDuplicate.size() + 1);
    const auto copy = std::find_if(
        saved.applications().cbegin(), saved.applications().cend(),
        [&idsBeforeDuplicate](const auto& application) {
            return !idsBeforeDuplicate.contains(application.id);
        });
    QVERIFY(copy != saved.applications().cend());
    QCOMPARE(copy->assignmentMode, createdAssignmentMode);
    QCOMPARE(copy->channelCount, createdChannelCount);
    QCOMPARE(copy->mappings.size(), createdMappingCount);
    const QString copyId = copy->id;

    auto* remove = window.findChild<QAction*>(QStringLiteral("deleteApplication:") + createdId);
    QVERIFY(remove != nullptr);
    QTimer::singleShot(50, &window, []() {
        auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(message != nullptr);
        message->button(QMessageBox::Yes)->click();
    });
    remove->trigger();
    QApplication::processEvents();
    QVERIFY2(ManagerModel::loadFromFile(workspace, &saved, &error), qPrintable(error));
    QVERIFY(saved.applicationById(createdId) == nullptr);
    QVERIFY(saved.applicationById(copyId) != nullptr);
}

void ManagerModelTest::projectWorkflowMutatesThroughWidgets() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString workspace = directory.filePath(QStringLiteral("projects.json"));
    QString memberApplicationId;
    {
        Ucal::Manager::MainWindow window(PresentationMode::Explanatory, UiLanguage::English,
                                      false, false, workspace);
        window.show();
        bool handled = false;
        QTimer::singleShot(50, &window, [&]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto* projects = dialog->findChild<QListWidget*>(QStringLiteral("projectList"));
            auto* applications = dialog->findChild<QListWidget*>(QStringLiteral("projectApplicationList"));
            auto* add = dialog->findChild<QPushButton*>(QStringLiteral("projectAddButton"));
            auto* rename = dialog->findChild<QPushButton*>(QStringLiteral("projectRenameButton"));
            QVERIFY(projects != nullptr);
            QVERIFY(applications != nullptr);
            QVERIFY(add != nullptr);
            QVERIFY(rename != nullptr);

            QTimer::singleShot(25, &window, []() {
                auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                QVERIFY(input != nullptr);
                input->setTextValue(QStringLiteral("Widget project"));
                input->accept();
            });
            add->click();
            QVERIFY(projects->currentItem() != nullptr);
            QVERIFY(projects->currentItem()->data(Qt::UserRole).toString() != QStringLiteral("default"));

            QTimer::singleShot(25, &window, []() {
                auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                QVERIFY(input != nullptr);
                QCOMPARE(input->textValue(), QStringLiteral("Widget project"));
                input->setTextValue(QStringLiteral("Widget project renamed"));
                input->accept();
            });
            rename->click();
            QVERIFY(applications->count() > 0);
            memberApplicationId = applications->item(0)->data(Qt::UserRole).toString();
            applications->item(0)->setCheckState(Qt::Checked);
            for (auto* button : dialog->findChildren<QPushButton*>()) {
                if (button->property("automationId").toString() ==
                    QStringLiteral("projectDoneButton")) {
                    handled = true;
                    button->click();
                    return;
                }
            }
            QFAIL("Project Done button was not found");
        });
        window.showDialogForReview(QStringLiteral("project"));
        QVERIFY(handled);
    }

    ManagerModel saved;
    QString error;
    QVERIFY2(ManagerModel::loadFromFile(workspace, &saved, &error), qPrintable(error));
    const auto project = std::find_if(
        saved.projects().cbegin(), saved.projects().cend(), [](const auto& item) {
            return item.name == QStringLiteral("Widget project renamed");
        });
    QVERIFY(project != saved.projects().cend());
    const QString projectId = project->id;
    QVERIFY(project->applicationIds.contains(memberApplicationId));

    {
        Ucal::Manager::MainWindow window(PresentationMode::Explanatory, UiLanguage::English,
                                      false, true, workspace);
        window.show();
        bool handled = false;
        QTimer::singleShot(50, &window, [&]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto* projects = dialog->findChild<QListWidget*>(QStringLiteral("projectList"));
            auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("projectDeleteButton"));
            QVERIFY(projects != nullptr);
            QVERIFY(remove != nullptr);
            for (int row = 0; row < projects->count(); ++row) {
                if (projects->item(row)->data(Qt::UserRole).toString() == projectId) {
                    projects->setCurrentRow(row);
                    break;
                }
            }
            QCOMPARE(projects->currentItem()->data(Qt::UserRole).toString(), projectId);
            QTimer::singleShot(0, [] { auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                QVERIFY(box); box->button(QMessageBox::Yes)->click(); });
            remove->click();
            for (auto* button : dialog->findChildren<QPushButton*>()) {
                if (button->property("automationId").toString() ==
                    QStringLiteral("projectDoneButton")) {
                    handled = true;
                    button->click();
                    return;
                }
            }
            QFAIL("Project Done button was not found");
        });
        window.showDialogForReview(QStringLiteral("project"));
        QVERIFY(handled);
    }
    QVERIFY2(ManagerModel::loadFromFile(workspace, &saved, &error), qPrintable(error));
    QVERIFY(saved.projectById(projectId) == nullptr);
    QVERIFY(saved.applicationById(memberApplicationId) != nullptr);
}

void ManagerModelTest::hardwareUsageIsReadOnly() {
    QTemporaryDir directory;
    const QString workspace = directory.filePath(QStringLiteral("hardware.json"));
    auto initial = ManagerModel::demo();
    QVERIFY(initial.replaceApplicationChannels(QStringLiteral("pcanview-bench"), {QString()}));
    QVERIFY(initial.saveToFile(workspace));
    bool inspected = false;
    Ucal::Manager::MainWindow window(PresentationMode::Explanatory, UiLanguage::English, false, true, workspace);
    QTimer::singleShot(50, &window, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        QTimer::singleShot(10000, dialog, &QDialog::reject);
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("hardwareApplicationList"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(list != nullptr); QVERIFY(buttons != nullptr);
        QVERIFY(buttons->button(QDialogButtonBox::Save) == nullptr);
        for (int row = 0; row < list->count(); ++row) {
            QVERIFY(!(list->item(row)->flags() & Qt::ItemIsUserCheckable));
            QVERIFY(list->item(row)->data(Qt::UserRole).toString() != QStringLiteral("pcanview-bench"));
        }
        inspected = true; buttons->button(QDialogButtonBox::Close)->click();
    });
    window.showDialogForReview(QStringLiteral("hardware-assignment"));
    QVERIFY(inspected);
    ManagerModel saved; QVERIFY(ManagerModel::loadFromFile(workspace, &saved));
    QCOMPARE(saved.effectiveMappingsForApplication(QStringLiteral("pcanview-bench")).first().endpointId, QString());
}

void ManagerModelTest::runtimeInventoryIsNotRestoredAsOnline() {
    QTemporaryDir metadataDirectory;
    QVERIFY(metadataDirectory.isValid());
    Ucal::Manager::BackendPackage providerA, providerB, native;
    providerA.id = "provider-a"; providerA.vendor = "Vendor A"; providerA.backendFamily = "j2534";
    providerB.id = "provider-b"; providerB.vendor = "Vendor B"; providerB.backendFamily = "j2534";
    providerA.manifestPath = createFile(metadataDirectory.filePath("a.json"), "{}");
    providerB.manifestPath = createFile(metadataDirectory.filePath("b.json"), "{}");
    native.id = "native"; native.vendor = "Native"; native.backendFamily = "native-family";
    native.manifestPath = createFile(metadataDirectory.filePath("native.json"),
        R"({"endpointPresentation":{"family":"Native API","handleFirst":81,"handleLast":95,"handleOffset":80}})");
    Ucal::EndpointDescriptor a, b, unknown, channelHandle;
    a.backendFamily = b.backendFamily = unknown.backendFamily = "j2534";
    a.backendInstanceId = "provider-a"; b.backendInstanceId = "provider-b";
    unknown.backendInstanceId = "unknown";
    channelHandle.backendFamily = "native-family"; channelHandle.physicalIndex = 81;
    const std::vector<Ucal::EndpointDescriptor> inventory{a, b, unknown, channelHandle};
    const auto projected = Ucal::Manager::projectRuntimeEndpoints(inventory, {providerA, providerB, native});
    const auto reordered = Ucal::Manager::projectRuntimeEndpoints(inventory, {native, providerB, providerA});
    QCOMPARE(projected.size(), 4);
    QCOMPARE(projected[0].vendor, QStringLiteral("Vendor A"));
    QCOMPARE(projected[1].vendor, QStringLiteral("Vendor B"));
    QCOMPARE(projected[2].vendor, QStringLiteral("Multi-vendor"));
    QCOMPARE(projected[3].physicalChannel, 1);
    QCOMPARE(projected[3].physicalIndex, quint32(81));
    for (int i = 0; i < projected.size(); ++i) QCOMPARE(projected[i].vendor, reordered[i].vendor);
    auto model = ManagerModel::demo();
    const auto boundId = model.endpointIdForChannel(1);
    auto transient = model.endpoints().first(); transient.id = QStringLiteral("unbound-hotplug");
    auto live = model.endpoints(); live << transient; model.reconcileInventory(live);
    const auto object = model.toJson();
    QVERIFY(!object.contains(QStringLiteral("inventoryCache")));
    for (const auto& value : object.value(QStringLiteral("endpointBindings")).toArray()) {
        QVERIFY(value.toObject().value(QStringLiteral("id")).toString() != transient.id);
        QVERIFY(!value.toObject().contains(QStringLiteral("state")));
    }
    ManagerModel restored; QVERIFY(ManagerModel::fromJson(object, &restored));
    QVERIFY(restored.endpointById(transient.id) == nullptr);
    QVERIFY(restored.endpointById(boundId) != nullptr);
    QCOMPARE(restored.endpointById(boundId)->state, EndpointState::Offline);
    QVERIFY(restored.endpointById(boundId)->historicalBinding);
    QVERIFY(restored.fullAccessMappings().isEmpty());
    restored.reconcileInventory({transient});
    QVERIFY(!restored.endpointById(transient.id)->historicalBinding);
    restored.reconcileInventory({});
    QVERIFY(restored.endpointById(transient.id) != nullptr);
    QVERIFY(!restored.endpointById(transient.id)->historicalBinding);
    QCOMPARE(restored.endpointById(transient.id)->state, EndpointState::Offline);
    ManagerModel restarted; QVERIFY(ManagerModel::fromJson(restored.toJson(), &restarted));
    QVERIFY(restarted.endpointById(transient.id) == nullptr);
    QCOMPARE(restarted.endpointIdForChannel(1), boundId);
    auto legacy = object; legacy.insert("schemaVersion", 2);
    legacy.insert("inventoryCache", QJsonArray{QJsonObject{{"id", "invalid"}}});
    // Legacy real records are migrated as references, never as an online catalog.
    legacy.insert("inventoryCache", object.value("endpointBindings"));
    legacy.insert("softwareInstallations", object.value("softwareBindings"));
    QVERIFY(ManagerModel::fromJson(legacy, &restarted));
    QVERIFY(restarted.endpointById(boundId)->historicalBinding);
    QTemporaryDir directory;
    const auto legacyFile = createFile(directory.filePath("legacy.json"), QJsonDocument(legacy).toJson());
    QVERIFY(ManagerModel::loadFromFile(legacyFile, &restarted));
    QCOMPARE(restarted.applications().size(), model.applications().size());
    QCOMPARE(restarted.endpointIdForChannel(1), boundId);
}

void ManagerModelTest::bridgeStopKeepsFailureVisible() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Ucal::Manager::MainWindow window(PresentationMode::Production, UiLanguage::English,
        false, false, directory.filePath(QStringLiteral("profiles.json")));
    window.model_ = ManagerModel::realisticDemo();
    const auto applications = window.model_.applicationsForProject(window.model_.activeProjectId());
    QVERIFY(!applications.isEmpty());
    const auto& profile = applications.first();
    const auto failureText = Ucal::Manager::uiText(UiLanguage::English, QStringLiteral("apps.lastFailure"));
    const auto stopText = Ucal::Manager::uiText(UiLanguage::English, QStringLiteral("apps.menu.stop"));
    qRegisterMetaType<QProcess::ExitStatus>("QProcess::ExitStatus");

    for (const int exitCode : {17, 0}) {
        Ucal::Manager::ProductLaunchSpec spec;
        spec.program = QCoreApplication::applicationFilePath();
        spec.workingDirectory = directory.path();
        spec.arguments = QStringList{QStringLiteral("--bridge-stop-probe"), QString::number(exitCode)};
        spec.environment = QProcessEnvironment::systemEnvironment();
        spec.resourceSlot = 1;
        window.startProfileProcess(profile.id, profile.name, spec, {});
        auto* process = window.activeLaunches_.value(profile.id).process.data();
        QVERIFY(process != nullptr);
        QSignalSpy finished(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished));
        QTRY_COMPARE(process->state(), QProcess::Running);

        QAction* stopAction = nullptr;
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->text() == stopText && action->isEnabled()) {
                stopAction = action;
                break;
            }
        }
        QVERIFY(stopAction != nullptr);
        stopAction->trigger();
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toInt(), exitCode);
        QCOMPARE(finished.first().at(1).value<QProcess::ExitStatus>(), QProcess::NormalExit);
        QVERIFY(!window.isProfileActive(profile.id));

        QFile log(directory.filePath(QStringLiteral("run-logs/") + profile.id + QStringLiteral(".last.log")));
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(log.readAll().contains("bridge received stop"));
        QCOMPARE(window.lastLaunchErrors_.contains(profile.id), exitCode != 0);
        if (exitCode != 0) {
            const auto details = window.lastLaunchErrors_.value(profile.id);
            QVERIFY(details.contains(QStringLiteral("exit 17")));
            QVERIFY(details.contains(QStringLiteral("bridge received stop")));
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto labels = window.findChildren<QLabel*>();
        QCOMPARE(std::any_of(labels.cbegin(), labels.cend(), [&failureText](const QLabel* label) {
            return label->text() == failureText;
        }), exitCode != 0);
    }
}

void ManagerModelTest::processWatchdogBoundsHungHelper() {
    const auto helper = QCoreApplication::applicationFilePath();

    QProcess hung;
    bool timedOut = false;
    hung.start(helper, QStringList{QStringLiteral("--watchdog-hang-probe")});
    QVERIFY(hung.waitForStarted(10000));
    Ucal::Manager::armProcessWatchdog(&hung, 100, [&timedOut]() { timedOut = true; });
    QTRY_COMPARE_WITH_TIMEOUT(hung.state(), QProcess::NotRunning, 3000);
    QVERIFY(timedOut);
    QVERIFY(hung.property("ucalTimedOut").toBool());

    QProcess failed;
    bool falseTimeout = false;
    failed.start(helper, QStringList{QStringLiteral("--watchdog-exit-probe")});
    QVERIFY(failed.waitForStarted(10000));
    Ucal::Manager::armProcessWatchdog(&failed, 5000,
                                   [&falseTimeout]() { falseTimeout = true; });
    QTRY_COMPARE_WITH_TIMEOUT(failed.state(), QProcess::NotRunning, 10000);
    QVERIFY(!falseTimeout);
    QCOMPARE(failed.exitCode(), 7);

    const QByteArray original = qgetenv("UCAL_MANAGER_TEST_HELPER_TIMEOUT_MS");
    qputenv("UCAL_MANAGER_TEST_HELPER_TIMEOUT_MS", QByteArray("1"));
    QCOMPARE(Ucal::Manager::boundedHelperTimeoutMilliseconds(
                 "UCAL_MANAGER_TEST_HELPER_TIMEOUT_MS", 30000), 100);
    qputenv("UCAL_MANAGER_TEST_HELPER_TIMEOUT_MS", QByteArray("999999"));
    QCOMPARE(Ucal::Manager::boundedHelperTimeoutMilliseconds(
                 "UCAL_MANAGER_TEST_HELPER_TIMEOUT_MS", 30000), 300000);
    if (original.isNull()) qunsetenv("UCAL_MANAGER_TEST_HELPER_TIMEOUT_MS");
    else qputenv("UCAL_MANAGER_TEST_HELPER_TIMEOUT_MS", original);
}

void ManagerModelTest::settingsControlsPersistThroughWidgets() {
    QStandardPaths::setTestModeEnabled(true);
    const Ucal::Manager::UserPreferences original = Ucal::Manager::loadUserPreferences();
    Ucal::Manager::UserPreferences baseline;
    baseline.autoStart = false;
    baseline.startMinimized = false;
    baseline.autoScan = true;
    baseline.closeBehavior = Ucal::Manager::CloseBehavior::HideToTray;
    QVERIFY(Ucal::Manager::saveUserPreferences(baseline));

    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Ucal::Manager::MainWindow window(
            PresentationMode::Production, UiLanguage::English, false, false,
            directory.filePath(QStringLiteral("settings.json")));
        auto* startMinimized =
            window.findChild<QCheckBox*>(QStringLiteral("startMinimizedPreference"));
        auto* autoScan = window.findChild<QCheckBox*>(QStringLiteral("autoScanPreference"));
        auto* closeBehavior =
            window.findChild<QComboBox*>(QStringLiteral("closeBehaviorPreference"));
        QVERIFY(startMinimized != nullptr);
        QVERIFY(autoScan == nullptr);
        QVERIFY(closeBehavior != nullptr);
        startMinimized->setChecked(true);

        closeBehavior->setCurrentIndex(static_cast<int>(Ucal::Manager::CloseBehavior::Exit));
    }
    const Ucal::Manager::UserPreferences saved = Ucal::Manager::loadUserPreferences();
    QVERIFY(saved.startMinimized);
    QCOMPARE(saved.autoScan, baseline.autoScan);
    QCOMPARE(saved.closeBehavior, Ucal::Manager::CloseBehavior::Exit);
    QVERIFY(!saved.autoStart);
    QVERIFY(Ucal::Manager::saveUserPreferences(original));
}

void ManagerModelTest::askClosePolicyHonorsBothChoices() {
    QStandardPaths::setTestModeEnabled(true);
    const Ucal::Manager::UserPreferences original = Ucal::Manager::loadUserPreferences();
    Ucal::Manager::UserPreferences preferences = original;
    preferences.autoStart = false;
    preferences.closeBehavior = Ucal::Manager::CloseBehavior::Ask;
    QVERIFY(Ucal::Manager::saveUserPreferences(preferences));
    {
        Ucal::Manager::MainWindow window(PresentationMode::Production, UiLanguage::English,
                                      false, false);
        window.show();
        QTimer::singleShot(50, &window, []() {
            auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(message != nullptr);
            message->button(QMessageBox::No)->click();
        });
        QVERIFY(!window.close());
        QVERIFY(window.isVisible());
        QTimer::singleShot(50, &window, []() {
            auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(message != nullptr);
            message->button(QMessageBox::Yes)->click();
        });
        QVERIFY(window.close());
        QVERIFY(!window.isVisible());
    }
    QVERIFY(Ucal::Manager::saveUserPreferences(original));
}

void ManagerModelTest::trayCloseHidesAndOpenRestores() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QSKIP("The target desktop does not expose a system tray.");
    }
    QStandardPaths::setTestModeEnabled(true);
    const Ucal::Manager::UserPreferences original = Ucal::Manager::loadUserPreferences();
    Ucal::Manager::UserPreferences preferences = original;
    preferences.autoStart = false;
    preferences.closeBehavior = Ucal::Manager::CloseBehavior::HideToTray;
    QVERIFY(Ucal::Manager::saveUserPreferences(preferences));
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Ucal::Manager::MainWindow window(
            PresentationMode::Production, UiLanguage::English, true, false,
            directory.filePath(QStringLiteral("tray.json")));
        window.show();
        QApplication::processEvents();
        auto* tray = window.findChild<QSystemTrayIcon*>();
        QVERIFY(tray != nullptr);
        QVERIFY(tray->isVisible());
        QVERIFY(tray->contextMenu() != nullptr);
        QVERIFY(tray->contextMenu()->actions().size() >= 5);

        QVERIFY(!window.close());
        QVERIFY(!window.isVisible());
        QVERIFY(tray->isVisible());

        QAction* open = tray->contextMenu()->actions().first();
        QVERIFY(open != nullptr);
        QVERIFY(open->isEnabled());
        open->trigger();
        QApplication::processEvents();
        QVERIFY(window.isVisible());
    }
    QVERIFY(Ucal::Manager::saveUserPreferences(original));
}

void ManagerModelTest::closeWithoutTrayExitsNormally() {
    Ucal::Manager::MainWindow window(PresentationMode::Realistic, UiLanguage::English,
                                  false, false);
    window.show();
    QVERIFY(window.isVisible());
    QVERIFY(window.close());
    QVERIFY(!window.isVisible());
}

int main(int argc, char** argv) {
    if (argc == 3 && QByteArray(argv[1]) == "--bridge-stop-probe") {
        std::string command;
        if (!std::getline(std::cin, command) || command != "stop") return 2;
        std::cerr << "bridge received stop\n";
        return QByteArray(argv[2]).toInt();
    }
    // Controlled subprocesses for watchdog assertions. Avoid shell profiles,
    // PowerShell cold-start time and GUI setup in this timing-sensitive probe.
    if (argc == 2 && QByteArray(argv[1]) == "--watchdog-exit-probe") return 7;
    if (argc == 2 && QByteArray(argv[1]) == "--watchdog-hang-probe") {
        QThread::sleep(30);
        return 0;
    }
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("UCAL Regression"));
    QCoreApplication::setApplicationName(QStringLiteral("Switch QtTest"));
    QStandardPaths::setTestModeEnabled(true);
    const auto arguments = application.arguments();
    // Harmless child target for the genuine Windows ShellExecute shortcut test.
    if (arguments.size() == 5 && arguments.at(1) == QStringLiteral("--profile") &&
        arguments.at(2) == QStringLiteral("shortcut-launch-probe") &&
        arguments.at(3) == QStringLiteral("--workspace")) {
        const QJsonObject result{{QStringLiteral("profile"), arguments.at(2)},
            {QStringLiteral("workspace"), arguments.at(4)},
            {QStringLiteral("directory"), QDir::currentPath()}};
        return createFile(arguments.at(4), QJsonDocument(result).toJson()).isEmpty() ? 1 : 0;
    }
    if (arguments.size()==5 && arguments.at(1)==QStringLiteral("--package-tool-probe")) {
        return createFile(arguments.at(2),QJsonDocument(QJsonObject{{"argument",arguments.at(3)},
            {"literal",arguments.at(4)},{"directory",QDir::currentPath()}}).toJson()).isEmpty()?1:0;
    }
    // Even a direct Qt Creator run must not read/write a user's portable state
    // or discover real hardware through a running product Broker.
    QTemporaryDir context(QDir::tempPath() + QStringLiteral("/ucal_qtest_XXXXXX"));
    if (!context.isValid() || !QDir().mkpath(context.filePath(QStringLiteral("backends")))) return 2;
    qputenv("UCAL_MANAGER_SETTINGS_DIRECTORY", context.path().toUtf8());
    qputenv("UCAL_BACKEND_DIRECTORY", context.filePath(QStringLiteral("backends")).toUtf8());
    qputenv("UCAL_BROKER_INSTANCE", (QStringLiteral("ucal_qtest_") +
        QUuid::createUuid().toString(QUuid::WithoutBraces)).toUtf8());
    ManagerModelTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ManagerModelTest.moc"
