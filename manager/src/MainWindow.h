#pragma once

#include "control/ManagerModel.h"
#include "control/UiPresentation.h"
#include "RuntimeDiscovery.h"
#include "control/UserPreferences.h"

#include <QFutureWatcher>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSharedPointer>

class QButtonGroup;
class QCloseEvent;
class QComboBox;
class QLabel;
class QProcess;
class QStackedWidget;
class QSystemTrayIcon;
class QTemporaryDir;
class QVBoxLayout;
class QWidget;
class ManagerModelTest;

namespace Ucal::Manager {

class HardwareChangeMonitor;
struct ProductLaunchSpec;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(PresentationMode presentationMode, UiLanguage language,
                        bool enableTray, bool loadPersistedState,
                        const QString& workspacePathOverride = QString(),
                        QWidget* parent = nullptr);
    ~MainWindow() override;
    void selectPage(int index);
    void launchProfile(const QString& applicationId);
    void handleExternalRequest(const QString& applicationId = QString());
    void showApplicationEditorForReview(int tabIndex = 0);
    void showDialogForReview(const QString& dialogName);

signals:
    void profileLaunchRequested(const QString& applicationId);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    friend class ::ManagerModelTest;

    struct ActiveLaunch {
        QPointer<QProcess> process;
        QSharedPointer<QTemporaryDir> temporaryDirectory;
        QString applicationName;
        QStringList evidenceFiles;
        int resourceSlot = 0;
        bool stopping = false;
    };

    void rebuildUserInterface(int selectedPage = -1);
    void changeUiLanguage(UiLanguage language);
    QWidget* createSidebar();
    QWidget* createApplicationsPage();
    QWidget* createHardwarePage();
    QWidget* createSoftwarePage();
    QWidget* createBackendsPage();
    QWidget* createSettingsPage();
    QWidget* createPageHeader(const QString& eyebrow, const QString& title,
                              const QString& subtitle, QWidget* trailing = nullptr);
    QWidget* createApplicationCard(const ApplicationProfile& application);

    void rebuildApplicationsList();
    void rebuildHardwarePage();
    void rebuildSoftwarePage();
    void rescanSoftwareInstallations();
    void rebuildBackendsPage();
    void refreshRuntimeInventory(bool notifyOnError = true, bool scanSoftware = true);
    void refreshProjectSelector();
    void showProjectManager();
    void showAddApplicationDialog();
    void showApplicationEditor(const QString& applicationId, int initialTab = 1, const QString& softwareId = QString());
    void showSoftwareApplications(const QString& softwareId);
    void rescanBackendPackages();
    void toggleBackendPackage(const QString& packageId);
    void showHardwareAssignmentDialog(const QString& endpointId);
    void showBackendPackageManager(const QString& packageId);
    void showDeveloperDeploymentDialog(const QString& applicationId);
    void runFrontendOperation(const QString& applicationId, const QString& operation,
                                const QJsonObject& extraContext = {});
    void createProfileShortcutAction(const QString& applicationId);
    void executeProfileLaunch(const QString& applicationId, bool nativeLaunch = false,
                              bool planOnly = false);
    void startProfileProcess(const QString& applicationId, const QString& applicationName,
                             const ProductLaunchSpec& spec,
                             const QSharedPointer<QTemporaryDir>& temporaryDirectory);
    void launchAllProfiles();
    void showActiveLaunch(const QString& applicationId);
    void stopActiveLaunch(const QString& applicationId);
    void runPendingProfileLaunches();
    void refreshInventoryAction(const QString& target);
    void saveWorkspace(bool notify = false);
    void savePreferences();

    void createTray();
    void rebuildTrayMenu();
    void showAndRaise();
    void updateStatusSummary(const QString& message = QString());
    QString t(const char* key) const;
    bool isRealistic() const;
    bool isProduction() const;
    bool usesProductCopy() const;
    bool isProfileActive(const QString& applicationId) const;
    QString projectDisplayName(const Project& project) const;
    QString endpointStateDisplay(EndpointState state) const;
    QString backendStatus(const BackendPackage& package) const;
    QString metadataDisplay(const QString& value) const;
    QString softwareCompatibilityDisplay(const SoftwareInstallation& software) const;
    QString integrationDisplay(const QString& adapterId) const;

    PresentationMode presentationMode_ = PresentationMode::Explanatory;
    UiLanguage language_ = UiLanguage::System;
    ManagerModel model_;
    QString workspacePath_;
    QStackedWidget* pages_ = nullptr;
    QWidget* hardwarePage_ = nullptr;
    QWidget* softwarePage_ = nullptr;
    QWidget* backendsPage_ = nullptr;
    QFutureWatcher<RuntimeDiscoveryResult>* discoveryWatcher_ = nullptr;
    HardwareChangeMonitor* hardwareChangeMonitor_ = nullptr;
    bool inventoryRefreshPending_ = false;
    bool inventorySoftwarePending_ = false;
    bool inventoryNotifyPending_ = false;
    QFutureWatcher<RuntimeDiscoveryResult>* softwareDiscoveryWatcher_ = nullptr;
    QButtonGroup* navigation_ = nullptr;
    QComboBox* projectSelector_ = nullptr;
    QVBoxLayout* applicationsList_ = nullptr;
    QLabel* projectSummary_ = nullptr;
    QLabel* statusSummary_ = nullptr;
    QLabel* connectionStatus_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    bool closeToTray_ = true;
    bool askBeforeExit_ = false;
    bool forceQuit_ = false;
    bool runtimeActionsEnabled_ = false;
    bool brokerInventoryFresh_ = false;
    quint64 backendCatalogGeneration_ = 0;
    QString hardwareFilter_;
    QStringList pendingLaunchProfileIds_;
    QHash<QString, ActiveLaunch> activeLaunches_;
    QHash<QString, QString> lastLaunchErrors_;
    QStringList backendHealth_;
    QMap<QString, RuntimeLibraryDetails> backendRuntimes_;
    UserPreferences preferences_;
};

}  // namespace Ucal::Manager
