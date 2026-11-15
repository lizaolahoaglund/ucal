#include "MainWindow.h"
#include "Branding.h"
#include "HardwareChangeMonitor.h"
#include "control/LaunchPlan.h"
#include "ProcessWatchdog.h"
#include "control/ProductLauncher.h"
#include "control/ProductOperations.h"
#include "control/PluginMetadata.h"
#include "control/BackendPlugins.h"
#include "control/FrontendPlugins.h"
#include "RuntimeDiscovery.h"
#include "ShortcutWriter.h"
#include "control/PortablePaths.h"
#include "control/SoftwareDiscovery.h"
#include "control/UserPreferences.h"

#include <ucal/Client.hpp>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <functional>

namespace Ucal::Manager {
namespace {

bool saveOperationEvidence(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
           file.commit();
}

QString operationEvidenceDirectory(const QString& workspacePath,
                                   QString applicationId, const QString& action) {
    applicationId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                          QStringLiteral("_"));
    while (applicationId.startsWith(QLatin1Char('.'))) {
        applicationId[0] = QLatin1Char('_');
    }
    if (applicationId.isEmpty()) applicationId = QStringLiteral("application");
    const QString stamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMddTHHmmsszzzZ"));
    return QDir(QFileInfo(workspacePath).absolutePath())
        .filePath(QStringLiteral("deployment-evidence/%1/%2-%3")
                      .arg(applicationId.left(96), stamp, action.toLower()));
}

QIcon navigationIcon(const int kind) {
    QIcon icon;
    for (const bool checked : {false, true}) {
        QPixmap image(40, 40);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(2, 2);
        painter.setPen(QPen(QColor(checked ? "#2457d6" : "#66758b"), 1.5,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (kind == 0) {
            for (int x : {3, 11}) for (int y : {3, 11}) painter.drawRoundedRect(x, y, 6, 6, 1, 1);
        } else if (kind == 2) {
            painter.drawRoundedRect(2, 3, 16, 11, 1.5, 1.5);
            painter.drawLine(10, 14, 10, 17); painter.drawLine(6, 17, 14, 17);
        } else if (kind == 3) {
            painter.drawRoundedRect(3, 3, 14, 6, 1.5, 1.5);
            painter.drawRoundedRect(3, 11, 14, 6, 1.5, 1.5);
            painter.drawPoint(6, 6); painter.drawPoint(6, 14);
            painter.drawLine(10, 6, 14, 6); painter.drawLine(10, 14, 14, 14);
        } else if (kind == 1) {
            painter.drawRoundedRect(5, 5, 10, 10, 2, 2);
            for (int n : {7, 10, 13}) {
                painter.drawLine(n, 2, n, 5); painter.drawLine(n, 15, n, 18);
                painter.drawLine(2, n, 5, n); painter.drawLine(15, n, 18, n);
            }
        } else {
            painter.translate(10, 10);
            painter.drawEllipse(QPointF(0, 0), 5.5, 5.5);
            painter.drawEllipse(QPointF(0, 0), 2, 2);
            for (int i = 0; i < 8; ++i) { painter.drawLine(0, -6, 0, -8); painter.rotate(45); }
        }
        painter.end();
        icon.addPixmap(image, QIcon::Normal, checked ? QIcon::On : QIcon::Off);
    }
    return icon;
}

QFrame* cardFrame(QWidget* parent = nullptr) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("card"));
    return frame;
}

class ApplicationEditorDialog final : public QDialog {
public:
    using QDialog::QDialog;
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (auto* button = qobject_cast<QPushButton*>(focusWidget())) {
                if (button->isEnabled()) button->click();
            }
            event->accept();
            return;
        }
        QDialog::keyPressEvent(event);
    }
};

QLabel* badgeLabel(const QString& text, const QString& kind, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("pill"));
    label->setProperty("kind", kind);
    label->setAlignment(Qt::AlignCenter);
    label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return label;
}

QLabel* mutedText(const QString& text, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

QLabel* sectionTitle(const QString& text, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QString stateKind(const EndpointState state) {
    if (state == EndpointState::Online) return QStringLiteral("success");
    if (state == EndpointState::Busy) return QStringLiteral("warning");
    return QStringLiteral("muted");
}

QString endpointDisplay(const Endpoint& endpoint, const QString& packageName = QString()) {
    QString identity = endpoint.model.startsWith(endpoint.vendor, Qt::CaseInsensitive)
        ? endpoint.model : QStringLiteral("%1 %2").arg(endpoint.vendor, endpoint.model);
    if (!endpoint.serial.isEmpty() && !endpoint.serial.startsWith(QStringLiteral("Not reported")) &&
        !identity.contains(endpoint.serial)) identity += QStringLiteral(" · SN %1").arg(endpoint.serial);
    const auto driver = !packageName.isEmpty() ? packageName : endpoint.backendName.isEmpty() ? endpoint.backendFamily : endpoint.backendName;
    return QStringLiteral("%1: %2 · %3 %4").arg(driver, identity, endpoint.busType).arg(endpoint.physicalChannel);
}

QString userLaunchSummary(const LaunchPlan& plan, const UiLanguage language) {
    const auto t = [language](const char* key) { return uiText(language, QString::fromLatin1(key)); };
    QStringList lines;
    lines << t("launch.summary.application").arg(plan.applicationName)
          << t("launch.summary.channelCount").arg(plan.endpoints.size())
          << t("launch.summary.status").arg(t(plan.ready ? "launch.summary.ready" : "launch.summary.blocked"));
    for (const LaunchEndpoint& endpoint : plan.endpoints) {
        lines << QStringLiteral("CAN %1 → %2 · CAN %3").arg(endpoint.logicalChannel)
            .arg(endpoint.serial.isEmpty() || endpoint.serial.startsWith(QStringLiteral("Not reported"))
                ? endpoint.backendFamily : QStringLiteral("SN %1").arg(endpoint.serial))
            .arg(endpoint.displayChannel);
    }
    if (!plan.blockers.isEmpty()) {
        lines << t("launch.summary.blockers");
        lines << plan.blockers;
    }
    return lines.join(QLatin1Char('\n'));
}

QString adapterDisplay(const Endpoint& endpoint) {
    return endpoint.model.startsWith(endpoint.vendor, Qt::CaseInsensitive)
        ? endpoint.model : QStringLiteral("%1 %2").arg(endpoint.vendor, endpoint.model);
}

bool isCanEndpoint(const Endpoint& endpoint) {
    return endpoint.busType.compare(QStringLiteral("CAN"), Qt::CaseInsensitive) == 0;
}

QString canOnlyCapabilities(const QString& capabilities) {
    QStringList result;
    const QStringList parts = capabilities.split(
        QRegularExpression(QStringLiteral("\\s*(?:/|,|·)\\s*")), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (!part.contains(QStringLiteral("LIN"), Qt::CaseInsensitive)) result.push_back(part);
    }
    return result.join(QStringLiteral(" / "));
}

bool profileAssignmentConfigured(const ApplicationProfile& application) {
    if (application.assignmentMode == AssignmentMode::FullAccess) return true;
    if (!duplicateEndpointChannels(application.mappings, application.channelCount).isEmpty()) return false;
    for (int channel = 1; channel <= application.channelCount; ++channel) {
        const auto mapping = std::find_if(
            application.mappings.cbegin(), application.mappings.cend(),
            [channel](const ChannelMapping& value) { return value.logicalChannel == channel; });
        if (mapping == application.mappings.cend() || mapping->endpointId.isEmpty()) return false;
    }
    return application.channelCount > 0;
}

bool profileConfigured(const ManagerModel& model, const ApplicationProfile& application) {
    const SoftwareInstallation* software = model.softwareById(application.softwareInstallationId);
    const auto frontend = software == nullptr ? FrontendPackage{} : frontendPackage(software->family);
    return software != nullptr && software->available &&
           software->integrationAdapters.contains(application.integrationAdapterId) &&
           (application.assignmentMode != AssignmentMode::FullAccess ||
            ((frontend.id.isEmpty() || frontend.fullAccess) &&
             model.effectiveMappingsForApplication(application.id).size() <= software->maximumCanChannels())) &&
           (application.assignmentMode != AssignmentMode::Selected ||
            application.channelCount <= software->maximumCanChannels()) &&
           profileAssignmentConfigured(application);
}

void clearLayout(QLayout* layout) {
    if (layout == nullptr) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (item->widget() != nullptr) { item->widget()->hide(); item->widget()->deleteLater(); }
        if (item->layout() != nullptr) clearLayout(item->layout());
        delete item;
    }
}

QString maskedSerial(const QString& serial) {
    if (serial.isEmpty() || serial.startsWith(QStringLiteral("Not reported"))) return QString();
    if (serial.size() <= 4) return serial;
    return QStringLiteral("***%1").arg(serial.right(4));
}

QString firstExistingFile(const QStringList& candidates) {
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).isFile()) return QFileInfo(candidate).absoluteFilePath();
    }
    return QString();
}

// The Switch is the control plane: when the Broker is not already running, start it
// with the enabled plugin catalog so discovery and product launches can proceed.
void startBrokerIfNeeded() {
    const Ucal::Client client(250u);
    QElapsedTimer deadline;
    deadline.start();
    if (client.ping() == Ucal::Status::Ok) return;

    const QString root = resolveProductRoot(QCoreApplication::applicationDirPath());
    if (root.isEmpty()) return;
    const QString broker = firstExistingFile({
        QDir(root).filePath(QStringLiteral("runtime/x64/UcalBroker.exe")),
        QDir(root).filePath(QStringLiteral("out/x64/Release/UcalBroker.exe"))});
    if (broker.isEmpty()) return;
    QProcess brokerProcess;
    brokerProcess.setProgram(broker);
    brokerProcess.setWorkingDirectory(QFileInfo(broker).absolutePath());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("UCAL_BACKEND"), QStringLiteral("PLUGINS"));
    environment.insert(QStringLiteral("UCAL_BACKEND_CATALOG"),backendPluginCatalogPath());
    brokerProcess.setProcessEnvironment(environment);
    qint64 pid = 0;
    const bool started = brokerProcess.startDetached(&pid);
    if (!started) return;

    while (deadline.elapsed() < 5000) {
        QThread::msleep(200);
        if (client.ping() == Ucal::Status::Ok) break;
    }
}

}  // namespace

MainWindow::MainWindow(const PresentationMode presentationMode, const UiLanguage language,
                       const bool enableTray, const bool loadPersistedState,
                       const QString& workspacePathOverride, QWidget* parent)
    : QMainWindow(parent), presentationMode_(presentationMode), language_(language),
      model_(presentationMode == PresentationMode::Realistic
                 ? ManagerModel::realisticDemo()
                 : presentationMode == PresentationMode::Production
                       ? ManagerModel::productionDefaults() : ManagerModel::demo()),
      workspacePath_(workspacePathOverride.isEmpty()
                         ? presentationMode == PresentationMode::Production
                               ? ManagerModel::productionStoragePath()
                               : ManagerModel::defaultStoragePath()
                         : QFileInfo(workspacePathOverride).absoluteFilePath()),
      runtimeActionsEnabled_(loadPersistedState && presentationMode == PresentationMode::Production),
      preferences_(loadUserPreferences()) {
    installStandardButtonTranslations(language_);
    closeToTray_ = preferences_.closeBehavior == CloseBehavior::HideToTray;
    askBeforeExit_ = preferences_.closeBehavior == CloseBehavior::Ask;
    QString loadError;
    if (loadPersistedState && !isRealistic()) {
        ManagerModel persisted;
        if (ManagerModel::loadFromFile(workspacePath_, &persisted, &loadError)) {
            model_ = persisted;
        } else if (!QFileInfo::exists(workspacePath_) &&
                   !QFileInfo::exists(workspacePath_ + QStringLiteral(".bak"))) {
            loadError.clear();
        }
    }

    if (isProduction()) {
        for (const auto& software : model_.softwareInstallations())
            if ((software.source == QStringLiteral("Manual") || software.source == QStringLiteral("手动添加")) &&
                software.family != QStringLiteral("custom")) rememberSoftwareSearchPath(software.executablePath);
        // The integration method is automatic. Resolve installed, complete
        // application-local assets rather than leaving a fixed combo on an
        // incomplete historical route.
        for (const auto& profile : model_.applications()) {
            const auto* software = model_.softwareById(profile.softwareInstallationId);
            if (software != nullptr && frontendPackage(software->family).refreshIntegration)
                model_.mutableApplicationById(profile.id)->integrationAdapterId = preferredInstalledAdapter(*software);
        }
        auto scan = scanBackendDirectory(backendDirectory(QCoreApplication::applicationDirPath()));
        for (auto& package : scan.packages) package.enabled = !preferences_.disabledBackendIds.contains(package.id);
        const auto pluginErrors=synchronizeBackendPlugins(scan.packages,QCoreApplication::applicationDirPath());
        if(!pluginErrors.isEmpty())scan.diagnostics<<pluginErrors;
        model_.replaceBackendPackages(scan.packages);
        for (const auto& package : scan.packages) model_.setBackendPackageEnabled(package.id, package.enabled);
        if (!scan.diagnostics.isEmpty()) {
            const QString logs = QDir(QFileInfo(workspacePath_).absolutePath()).filePath(QStringLiteral("run-logs"));
            QDir().mkpath(logs);
            saveOperationEvidence(QDir(logs).filePath(QStringLiteral("backend-scan.log")),
                                  scan.diagnostics.join(QLatin1Char('\n')).toUtf8());
        }
    }

    setWindowTitle(t(usesProductCopy() ? "window.realistic" : "window.explanatory"));
    setWindowIcon(managerIcon());
    setStyleSheet(QStringLiteral(R"QSS(
        * { font-family: "Microsoft YaHei UI", "Segoe UI"; font-size: 13px; color: #1d2939; }
        QMainWindow, QWidget#root, QStackedWidget, QDialog#backendPluginSettings, QDialog#backendAddPlugin { background: #f5f7fb; }
        QFrame#sidebar { background: #ffffff; border-right: 1px solid #e5e9f0; }
        QLabel#brand { font-size: 20px; font-weight: 750; color: #172033; }
        QPushButton#navButton { border: 0; border-radius: 9px; text-align: left; padding: 11px 14px; color: #5d6676; background: transparent; }
        QPushButton#navButton:hover { background: #f2f5fb; color: #263247; }
        QPushButton#navButton:checked { background: #eaf0ff; color: #2457d6; font-weight: 650; }
        QLabel#eyebrow { color: #356cf6; font-weight: 700; font-size: 10px; }
        QLabel#pageTitle { color: #172033; font-weight: 750; font-size: 25px; }
        QLabel#pageSubtitle, QLabel#muted { color: #738095; }
        QLabel#sectionTitle { color: #273248; font-weight: 700; font-size: 15px; }
        QLabel#cardTitle { color: #172033; font-weight: 700; font-size: 16px; }
        QFrame#card, QFrame#applicationQuickStart { background: #ffffff; border: 1px solid #e3e8f0; border-radius: 13px; }
        QFrame#card:hover { border: 1px solid #c8d5f6; }
        QFrame#softPanel { background: #f7f9fd; border: 1px solid #e6eaf1; border-radius: 10px; }
        QLabel#letterBadge { min-width: 44px; max-width: 44px; min-height: 44px; max-height: 44px; border-radius: 12px; background: #eaf0ff; color: #356cf6; font-size: 17px; font-weight: 800; }
        QLabel#pill { border-radius: 9px; padding: 3px 9px; font-size: 10px; font-weight: 650; }
        QLabel#pill[kind="success"] { background: #e5f7ee; color: #16794b; }
        QLabel#pill[kind="warning"] { background: #fff3d9; color: #986000; }
        QLabel#pill[kind="info"] { background: #eaf0ff; color: #2f5fd0; }
        QLabel#pill[kind="muted"] { background: #eef1f5; color: #667085; }
        QPushButton#primaryButton, QToolButton#primaryButton { background: #356cf6; color: white; border: 0; border-radius: 9px; padding: 9px 17px; font-weight: 650; }
        QPushButton#primaryButton:hover, QToolButton#primaryButton:hover { background: #285bd8; }
        QPushButton#secondaryButton, QToolButton#secondaryButton { background: #ffffff; color: #344054; border: 1px solid #d7dce5; border-radius: 9px; padding: 8px 13px; }
        QPushButton#secondaryButton:hover, QToolButton#secondaryButton:hover { background: #f6f8fc; border-color: #bac4d6; }
        QPushButton#backendEnabledButton { background: #16804a; color: white; border: 1px solid #16804a; border-radius: 9px; padding: 8px 16px; font-weight: 650; }
        QPushButton#backendEnabledButton:hover { background: #126b3d; border-color: #126b3d; }
        QPushButton#backendDisabledButton { background: #d92d20; color: white; border: 1px solid #d92d20; border-radius: 9px; padding: 8px 16px; font-weight: 650; }
        QPushButton#backendDisabledButton:hover { background: #b42318; border-color: #b42318; }
        QPushButton#ghostButton, QToolButton#ghostButton { border: 0; background: transparent; color: #46617f; padding: 7px 9px; border-radius: 7px; }
        QPushButton#ghostButton:hover, QToolButton#ghostButton:hover { background: #eef3fb; }
        QPushButton#addCard { background: rgba(255,255,255,0.72); border: 1px dashed #aebbd0; border-radius: 13px; padding: 18px; color: #456386; font-weight: 650; }
        QPushButton#addCard:hover { background: #ffffff; border-color: #6d8fd8; color: #2457d6; }
        QPushButton#addChannelButton { background: #ffffff; border: 1px dashed #aebbd0; border-radius: 8px; padding: 9px; color: #456386; }
        QPushButton#addChannelButton:hover { background: #eef3fb; border-color: #6d8fd8; }
        QPushButton#addChannelButton:disabled { background: #f2f4f7; color: #98a2b3; }
        QPushButton#primaryButton:disabled, QToolButton#primaryButton:disabled,
        QPushButton#secondaryButton:disabled, QToolButton#secondaryButton:disabled,
        QPushButton#ghostButton:disabled, QToolButton#ghostButton:disabled {
            background: #f2f4f7; color: #98a2b3; border-color: #e3e8f0;
        }
        QComboBox, QSpinBox, QLineEdit { background: white; border: 1px solid #d7dce5; border-radius: 8px; padding: 7px 10px; min-height: 18px; }
        QToolButton#ghostButton::menu-indicator { image: none; width: 0px; }
        QMenu { background: white; border: 1px solid #d7dce5; padding: 5px; }
        QMenu::item { padding: 7px 22px; }
        QMenu::item:selected { background: #eaf0ff; color: #2457d6; }
        QMenu::item:disabled { color: #98a2b3; }
        QMenu::separator { height: 1px; background: #e3e8f0; margin: 5px 10px; }
        QComboBox:disabled { background: #f2f4f7; color: #667085; }
        QPushButton#assignedButton { background: #eaf0ff; color: #2457d6; border: 1px solid #b9ccfb; border-radius: 9px; padding: 8px 13px; }
        QPushButton#assignedButton:hover { background: #dae6ff; }
        QComboBox:focus, QSpinBox:focus, QLineEdit:focus { border: 1px solid #6b8ff3; }
        QTableWidget, QListWidget { background: white; border: 1px solid #e3e8f0; border-radius: 10px; gridline-color: #edf0f5; selection-background-color: #edf2ff; selection-color: #1d2939; }
        QHeaderView::section { background: #f7f8fb; color: #667085; border: 0; border-bottom: 1px solid #e4e8f0; padding: 9px; font-weight: 600; }
        QTabWidget::pane { border: 1px solid #e3e8f0; border-radius: 10px; background: white; top: -1px; }
        QTabBar::tab { padding: 9px 18px; color: #667085; }
        QTabBar::tab:selected { color: #2457d6; font-weight: 650; }
        QGroupBox { border: 1px solid #e3e8f0; border-radius: 10px; margin-top: 10px; padding-top: 10px; font-weight: 650; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; }
        QStatusBar { background: #ffffff; border-top: 1px solid #e7eaf0; }
        QStatusBar QLabel { color: #667085; padding: 3px 8px; }
        QScrollArea { border: 0; background: transparent; }
        QScrollArea > QWidget > QWidget { background: transparent; }
    )QSS"));

    connectionStatus_ = new QLabel(this);
    connectionStatus_->setObjectName(QStringLiteral("connectionStatus"));
    statusBar()->addPermanentWidget(connectionStatus_);
    statusSummary_ = new QLabel(this);
    statusSummary_->setObjectName(QStringLiteral("statusSummary"));
    statusSummary_->setMinimumWidth(260);
    statusBar()->addPermanentWidget(statusSummary_);
    statusBar()->showMessage(t("status.ready"));
    rebuildUserInterface();
    // availableGeometry is in logical pixels. Obtain the native frame margins
    // before choosing a client size so scaled windows fit the whole work area.
    ensurePolished();
    (void)winId();
    if (auto* nativeWindow = windowHandle()) {
        if (auto* display = nativeWindow->screen()) {
            const auto available = display->availableGeometry();
            const auto margins = nativeWindow->frameMargins();
            const auto frame = QSize(margins.left() + margins.right(),
                                     margins.top() + margins.bottom());
            const auto capacity = (available.size() - frame).expandedTo(QSize(1, 1));
            setMinimumSize(QSize(1040, 700).boundedTo(capacity));
            resize(QSize(1280, 900).boundedTo(capacity));
            const auto outer = size() + frame;
            move(available.left() + (available.width() - outer.width()) / 2,
                 available.top() + (available.height() - outer.height()) / 2);
        }
    }
    if (enableTray && QSystemTrayIcon::isSystemTrayAvailable()) createTray();
    if (runtimeActionsEnabled_) {
        hardwareChangeMonitor_ = new HardwareChangeMonitor(this);
        connect(hardwareChangeMonitor_, &HardwareChangeMonitor::changeObserved, this,
                [this] { brokerInventoryFresh_ = false; });
        connect(hardwareChangeMonitor_, &HardwareChangeMonitor::refreshRequested, this,
                [this] { refreshRuntimeInventory(false, false); });
    }
    if (runtimeActionsEnabled_) {
        QTimer::singleShot(0, this, [this]() { refreshRuntimeInventory(false); });
    }
    if (!loadError.isEmpty()) {
        QTimer::singleShot(0, this, [this, loadError]() {
            QMessageBox::warning(this, QStringLiteral("UCAL Manager"),
                                 t("config.loadFailed").arg(loadError));
        });
    }
}

QString MainWindow::t(const char* key) const {
    return uiText(language_, QString::fromLatin1(key));
}

MainWindow::~MainWindow() {
    QVector<QPointer<QProcess>> processes;
    for (auto launch = activeLaunches_.begin(); launch != activeLaunches_.end(); ++launch) {
        if (launch->process.isNull() || launch->process->state() == QProcess::NotRunning) continue;
        launch->stopping = true;
        processes.push_back(launch->process);
        if (launch->resourceSlot > 0) launch->process->write("stop\n");
        else launch->process->terminate();
    }
    QElapsedTimer deadline;
    deadline.start();
    for (const auto& process : processes) {
        if (process.isNull() || process->state() == QProcess::NotRunning) continue;
        if (!process->waitForFinished(std::max(1, 6000 - static_cast<int>(deadline.elapsed()))))
            process->kill();
    }
}

void MainWindow::rebuildUserInterface(const int selectedPage) {
    const int page = selectedPage >= 0 ? selectedPage
                                      : pages_ == nullptr ? 0 : pages_->currentIndex();
    if (QWidget* previous = takeCentralWidget()) delete previous;
    pages_ = nullptr;
    hardwarePage_ = nullptr;
    softwarePage_ = nullptr;
    backendsPage_ = nullptr;
    navigation_ = nullptr;
    projectSelector_ = nullptr;
    applicationsList_ = nullptr;
    projectSummary_ = nullptr;

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("root"));
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(createSidebar());

    pages_ = new QStackedWidget(root);
    pages_->setObjectName(QStringLiteral("mainPages"));
    pages_->addWidget(createApplicationsPage());
    hardwarePage_ = createHardwarePage();
    pages_->addWidget(hardwarePage_);
    softwarePage_ = createSoftwarePage();
    pages_->addWidget(softwarePage_);
    backendsPage_ = createBackendsPage();
    pages_->addWidget(backendsPage_);
    pages_->addWidget(createSettingsPage());
    rootLayout->addWidget(pages_, 1);
    setCentralWidget(root);

    setWindowTitle(t(usesProductCopy() ? "window.realistic" : "window.explanatory"));
    refreshProjectSelector();
    rebuildApplicationsList();
    selectPage(std::max(0, std::min(page, pages_->count() - 1)));
    updateStatusSummary();
    if (tray_ != nullptr) {
        tray_->setToolTip(t(usesProductCopy() ? "window.realistic" : "window.explanatory"));
        rebuildTrayMenu();
    }
}

void MainWindow::changeUiLanguage(const UiLanguage language) {
    if (language_ == language) return;
    language_ = language;
    installStandardButtonTranslations(language_);
    if (!saveUiLanguagePreference(language_)) {
        QMessageBox::warning(this, QStringLiteral("UCAL Manager"),
                             t("settings.saveFailed"));
    }
    rebuildUserInterface();
}

bool MainWindow::isRealistic() const {
    return presentationMode_ == PresentationMode::Realistic;
}

bool MainWindow::isProduction() const {
    return presentationMode_ == PresentationMode::Production;
}

bool MainWindow::usesProductCopy() const {
    return isProduction() || isRealistic();
}

bool MainWindow::isProfileActive(const QString& applicationId) const {
    const auto launch = activeLaunches_.constFind(applicationId);
    return launch != activeLaunches_.cend() && !launch->process.isNull() &&
           launch->process->state() != QProcess::NotRunning;
}

QString MainWindow::projectDisplayName(const Project& project) const {
    if (project.id == QStringLiteral("default")) return t("project.default");
    if (project.id == QStringLiteral("powertrain")) return t("project.powertrain");
    if (project.id == QStringLiteral("gateway")) return t("project.gateway");
    return project.name;
}

QString MainWindow::endpointStateDisplay(const EndpointState state) const {
    switch (state) {
    case EndpointState::Online: return t("common.online");
    case EndpointState::Busy: return t("common.busy");
    case EndpointState::Offline: return t("common.offline");
    case EndpointState::DriverMissing: return t("common.attention");
    case EndpointState::Incompatible: return t("common.attention");
    case EndpointState::Disabled: return t("backends.disabledEndpoint");
    }
    return t("common.attention");
}

QString MainWindow::metadataDisplay(const QString& value) const {
    if (value == QStringLiteral("Windows Registry")) return t("source.registry");
    if (value == QStringLiteral("Windows App Paths")) return t("source.appPaths");
    if (value == QStringLiteral("Manual") || value == QStringLiteral("手动添加")) {
        return t("source.manual");
    }
    if (value == QStringLiteral("Built-in")) return t("source.builtin");
    if (value == QStringLiteral("Third-party package")) return t("source.thirdParty");
    if (value == QStringLiteral("Ready")) return t("source.ready");
    if (value.isEmpty() || value.startsWith(QStringLiteral("Not reported"))) return t("metadata.notReported");
    QString display = value;
    if (resolvedUiLanguage(language_) == UiLanguage::SimplifiedChinese) {
        display.replace(QStringLiteral("Listen-only"), QStringLiteral("只听"));
        display.replace(QStringLiteral("120 ohm termination"), QStringLiteral("120 Ω 终端"));
        display.replace(QStringLiteral("No-ACK"), QStringLiteral("无应答模式"));
        display.replace(QStringLiteral("Tx completion"), QStringLiteral("发送完成确认"));
    }
    return display;
}

QString MainWindow::integrationDisplay(const QString& id) const {
    const auto packages = scanFrontendDirectory(frontendDirectory(QCoreApplication::applicationDirPath())).packages;
    for (const auto& package : packages) {
        const auto name = localizedMetadata(package.integrations.value(id), language_);
        if (!name.isEmpty()) return name;
    }
    return id.isEmpty() ? t("common.needsConfiguration") : id;
}

QString MainWindow::softwareCompatibilityDisplay(const SoftwareInstallation& software) const {
    if (!software.available) return t("software.compat.missing");
    const auto package = frontendPackage(software.family);
    const bool supported = std::any_of(software.integrationAdapters.cbegin(), software.integrationAdapters.cend(),
        [](const QString& id) { return id != QStringLiteral("native"); });
    const auto text = localizedMetadata(supported ? package.compatibilityReady : package.compatibilityUnavailable, language_);
    return text.isEmpty() ? software.compatibility : text;
}

void MainWindow::selectPage(const int index) {
    if (pages_ == nullptr || index < 0 || index >= pages_->count()) return;
    pages_->setCurrentIndex(index);
    if (navigation_ != nullptr && navigation_->button(index) != nullptr) navigation_->button(index)->setChecked(true);
}

void MainWindow::showApplicationEditorForReview(const int tabIndex) {
    const auto applications = model_.applicationsForProject(model_.activeProjectId());
    if (!applications.isEmpty()) showApplicationEditor(applications.first().id, tabIndex);
}

void MainWindow::showDialogForReview(const QString& dialogName) {
    if (dialogName == QStringLiteral("project")) {
        showProjectManager();
    } else if (dialogName == QStringLiteral("add-application")) {
        showAddApplicationDialog();
    } else if (dialogName == QStringLiteral("hardware-assignment") && !model_.endpoints().isEmpty()) {
        showHardwareAssignmentDialog(model_.endpoints().first().id);
    }
}

QWidget* MainWindow::createSidebar() {
    auto* sidebar = new QFrame(this);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(20, 22, 20, 18);
    layout->setSpacing(8);

    auto* identity = new QWidget(sidebar);
    auto* identityLayout = new QHBoxLayout(identity);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(8);
    auto* logo = new QLabel(identity);
    logo->setObjectName(QStringLiteral("brandLogo"));
    logo->setPixmap(managerIcon().pixmap(36, 36));
    logo->setFixedSize(36, 36);
    auto* brand = new QLabel(QStringLiteral("UCAL Manager"), identity);
    brand->setObjectName(QStringLiteral("brand"));
    identityLayout->addWidget(logo);
    identityLayout->addWidget(brand);
    identityLayout->addStretch();
    layout->addWidget(identity);
    layout->addSpacing(18);
    auto* projectHeader = new QWidget(sidebar);
    auto* projectHeaderLayout = new QHBoxLayout(projectHeader);
    projectHeaderLayout->setContentsMargins(0, 0, 0, 0);
    projectHeaderLayout->setSpacing(6);
    projectHeaderLayout->addWidget(mutedText(t("project.current"), projectHeader));
    projectHeaderLayout->addStretch();
    auto* manage = new QToolButton(projectHeader);
    manage->setText(t("project.manage"));
    manage->setObjectName(QStringLiteral("secondaryButton"));
    manage->setProperty("automationId", QStringLiteral("projectManageButton"));
    manage->setAccessibleName(t("project.manage"));
    projectHeaderLayout->addWidget(manage);
    layout->addWidget(projectHeader);
    projectSelector_ = new QComboBox(sidebar);
    projectSelector_->setObjectName(QStringLiteral("projectSelector"));
    projectSelector_->setAccessibleName(t("project.current"));
    projectSelector_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(projectSelector_);
    layout->addSpacing(15);

    navigation_ = new QButtonGroup(sidebar);
    navigation_->setExclusive(true);
    const QStringList labels = {t("nav.applications"), t("nav.hardware"), t("nav.software"), t("nav.backends")};
    const QStringList automationIds = {
        QStringLiteral("navApplications"), QStringLiteral("navHardware"),
        QStringLiteral("navSoftware"), QStringLiteral("navBackends")};
    for (const int index : {0, 2, 3, 1}) {
        auto* button = new QPushButton(labels.at(index), sidebar);
        button->setObjectName(QStringLiteral("navButton"));
        button->setProperty("automationId", automationIds.at(index));
        button->setAccessibleName(labels.at(index));
        button->setIcon(navigationIcon(index));
        button->setIconSize(QSize(20, 20));
        button->setCheckable(true);
        navigation_->addButton(button, index);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, index]() { pages_->setCurrentIndex(index); });
        if (index == 0) button->setChecked(true);
    }
    layout->addStretch();
    auto* settings = new QPushButton(t("nav.settings"), sidebar);
    settings->setObjectName(QStringLiteral("navButton"));
    settings->setProperty("automationId", QStringLiteral("navSettings"));
    settings->setAccessibleName(t("nav.settings"));
    settings->setIcon(navigationIcon(4));
    settings->setIconSize(QSize(20, 20));
    settings->setCheckable(true);
    navigation_->addButton(settings, 4);
    layout->addWidget(settings);
    connect(settings, &QPushButton::clicked, this, [this]() { pages_->setCurrentIndex(4); });
    layout->addSpacing(10);
    auto* version = new QWidget(sidebar);
    version->setObjectName(QStringLiteral("sidebarVersion"));
    auto* versionLayout = new QHBoxLayout(version);
    versionLayout->setContentsMargins(14, 0, 14, 0);
    versionLayout->setSpacing(4);
    auto* versionIcon = new QLabel(version);
    versionIcon->setPixmap(navigationIcon(0).pixmap(20, 20));
    versionLayout->addWidget(versionIcon);
    auto* versionLabel = mutedText(QStringLiteral("UCAL Manager 0.9.1"), version);
    versionLabel->setObjectName(QStringLiteral("sidebarVersionLabel"));
    versionLabel->setWordWrap(false);
    auto versionFont = versionLabel->font();
    versionFont.setPixelSize(13);
    versionLabel->setFont(versionFont);
    versionLayout->addWidget(versionLabel);
    versionLayout->addStretch();
    layout->addWidget(version);
    brand->ensurePolished();
    versionLabel->ensurePolished();
    const int brandWidth = 40 + 36 + 8 + brand->fontMetrics().horizontalAdvance(brand->text());
    const int versionWidth = 40 + 28 + 20 + 4 +
        versionLabel->fontMetrics().horizontalAdvance(versionLabel->text());
    int sidebarWidth = qMax(254, qMax(brandWidth, versionWidth));
    for (const auto* button : navigation_->buttons())
        sidebarWidth = qMax(sidebarWidth, 40 + button->sizeHint().width());
    sidebar->setFixedWidth(sidebarWidth);


    connect(manage, &QToolButton::clicked, this, &MainWindow::showProjectManager);
    connect(projectSelector_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](const int index) {
        if (index < 0) return;
        if (model_.setActiveProject(projectSelector_->itemData(index).toString())) {
            rebuildApplicationsList();
            rebuildTrayMenu();
            saveWorkspace();
        }
    });
    return sidebar;
}

QWidget* MainWindow::createPageHeader(const QString& eyebrow, const QString& title,
                                      const QString& subtitle, QWidget* trailing) {
    auto* wrapper = new QWidget(this);
    auto* layout = new QHBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);
    auto* text = new QWidget(wrapper);
    auto* textLayout = new QVBoxLayout(text);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(4);
    auto* eyebrowLabel = new QLabel(eyebrow, text);
    eyebrowLabel->setObjectName(QStringLiteral("eyebrow"));
    auto* titleLabel = new QLabel(title, text);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(subtitle, text);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    subtitleLabel->setWordWrap(true);
    if (!eyebrow.isEmpty()) textLayout->addWidget(eyebrowLabel);
    else eyebrowLabel->hide();
    textLayout->addWidget(titleLabel);
    textLayout->addWidget(subtitleLabel);
    layout->addWidget(text, 1);
    if (trailing != nullptr) layout->addWidget(trailing, 0, Qt::AlignBottom);
    return wrapper;
}

QWidget* MainWindow::createApplicationsPage() {
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(34, 28, 34, 26);
    pageLayout->setSpacing(16);

    int onlineEndpoints = 0;
    for (const Endpoint& endpoint : model_.endpoints()) {
        if (isCanEndpoint(endpoint) && endpoint.state == EndpointState::Online) {
            ++onlineEndpoints;
        }
    }
    const QString applicationsSubtitle = isProduction()
        ? t("apps.subtitle.production")
              .arg(model_.applications().size())
        : t(isRealistic() ? "apps.subtitle.real" : "apps.subtitle.explain");
    pageLayout->addWidget(createPageHeader(t("apps.eyebrow"), t("nav.applications"),
                                           applicationsSubtitle));
    if (auto* header = page->findChild<QLabel*>(QStringLiteral("pageSubtitle")))
        header->setObjectName(QStringLiteral("applicationInventorySummary"));

    auto* projectBar = cardFrame(page);
    auto* projectBarLayout = new QHBoxLayout(projectBar);
    projectBarLayout->setContentsMargins(17, 13, 17, 13);
    projectSummary_ = new QLabel(projectBar);
    projectSummary_->setObjectName(QStringLiteral("sectionTitle"));
    projectBarLayout->addWidget(projectSummary_, 1);
    auto* startAll = new QToolButton(projectBar);
    startAll->setText(t("apps.startAll"));
    startAll->setObjectName(QStringLiteral("secondaryButton"));
    startAll->setProperty("ucalAction", QStringLiteral("startAll"));
    projectBarLayout->addWidget(startAll);
    pageLayout->addWidget(projectBar);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    applicationsList_ = new QVBoxLayout(content);
    applicationsList_->setContentsMargins(0, 2, 0, 2);
    applicationsList_->setSpacing(10);
    scroll->setWidget(content);
    pageLayout->addWidget(scroll, 1);

    connect(startAll, &QToolButton::clicked, this, &MainWindow::launchAllProfiles);
    return page;
}

QWidget* MainWindow::createApplicationCard(const ApplicationProfile& application) {
    const SoftwareInstallation* software = model_.softwareById(application.softwareInstallationId);
    const bool configured = profileConfigured(model_, application);
    auto* card = cardFrame(this);
    auto* layout = new QHBoxLayout(card);
    layout->setContentsMargins(18, 16, 16, 16);
    layout->setSpacing(14);
    auto* icon = new QLabel(application.name.left(1).toUpper(), card);
    icon->setObjectName(QStringLiteral("letterBadge"));
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);

    auto* text = new QWidget(card);
    auto* textLayout = new QVBoxLayout(text);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(4);
    auto* titleRow = new QWidget(text);
    auto* titleLayout = new QHBoxLayout(titleRow);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(8);
    auto* title = new QLabel(application.name, titleRow);
    title->setObjectName(QStringLiteral("cardTitle"));
    titleLayout->addWidget(title);
    titleLayout->addWidget(badgeLabel(usesProductCopy()
                                          ? (isProfileActive(application.id)
                                                 ? t("common.running")
                                                 : t(configured ? "common.configured"
                                                                : "common.needsConfiguration"))
                                          : application.maturity,
                                      isProfileActive(application.id) ? QStringLiteral("info")
                                                                      : configured
                                                                          ? QStringLiteral("success")
                                                                          : QStringLiteral("warning"),
                                      titleRow));
    titleLayout->addStretch();
    textLayout->addWidget(titleRow);
    const QString softwareLine = software == nullptr
        ? t("common.softwareUnavailable")
        : QStringLiteral("%1 %2 · %3").arg(software->displayName, software->version,
                                           software->architecture);
    textLayout->addWidget(mutedText(softwareLine, text));

    QString assignment;
    if (application.assignmentMode == AssignmentMode::FullAccess) {
        const int online = model_.effectiveMappingsForApplication(application.id).size();
        assignment = t("apps.fullSummary").arg(online);
    } else {
        int online = 0;
        for (const ChannelMapping& mapping : application.mappings) {
            const Endpoint* endpoint = model_.endpointById(mapping.endpointId);
            if (endpoint != nullptr && isCanEndpoint(*endpoint) &&
                endpoint->state == EndpointState::Online) ++online;
        }
        assignment = t("apps.selectedSummary").arg(application.channelCount).arg(online);
    }
    textLayout->addWidget(mutedText(assignment, text));
    if (lastLaunchErrors_.contains(application.id))
        textLayout->addWidget(mutedText(t("apps.lastFailure"), text));
    layout->addWidget(text, 1);

    auto* channelButton = new QPushButton(application.assignmentMode == AssignmentMode::FullAccess
                                              ? t("apps.allChannels")
                                              : t("apps.channelCount").arg(application.channelCount), card);
    channelButton->setObjectName(QStringLiteral("ghostButton"));
    layout->addWidget(channelButton);

    auto* settings = new QPushButton(t("common.settings"), card);
    settings->setObjectName(QStringLiteral("secondaryButton"));
    settings->setProperty("applicationSettings", application.id);

    auto* launch = new QPushButton(t("common.launch"), card);
    launch->setObjectName(QStringLiteral("primaryButton"));
    layout->addWidget(launch);
    layout->addWidget(settings);

    auto* more = new QToolButton(card);
    more->setText(QStringLiteral("•••"));
    more->setObjectName(QStringLiteral("ghostButton"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more);
    auto* native = moreMenu->addAction(t("apps.menu.native"));
    auto* currentProcess = moreMenu->addAction(t("apps.menu.last"));
    auto* stopProfile = moreMenu->addAction(t("apps.menu.stop"));
    stopProfile->setToolTip(t("apps.stopHelp"));
    currentProcess->setEnabled(isProfileActive(application.id));
    stopProfile->setEnabled(isProfileActive(application.id));
    moreMenu->addSeparator();
    auto* shortcut = moreMenu->addAction(t("apps.menu.shortcut"));
    shortcut->setObjectName(QStringLiteral("createShortcut:") + application.id);
    auto* duplicate = moreMenu->addAction(t("apps.menu.duplicate"));
    duplicate->setObjectName(QStringLiteral("duplicateApplication:") + application.id);
    auto* probe = moreMenu->addAction(t("apps.menu.check"));
    auto* location = moreMenu->addAction(t("apps.menu.location"));
    QAction* deployment = nullptr;
    const auto frontend = software == nullptr ? FrontendPackage{} : frontendPackage(software->family);
    const bool pluginDeployment = frontend.operations.contains(QStringLiteral("deploy"));
    if (pluginDeployment || !usesProductCopy()) {
        deployment = moreMenu->addAction(t("apps.menu.deploy"));
        deployment->setEnabled(pluginDeployment || !isProduction());
        if (!deployment->isEnabled()) deployment->setToolTip(t("packages.operationUnavailable"));
    }
    if (frontend.operations.contains(QStringLiteral("restore_deployment"))) {
        auto* restore = moreMenu->addAction(t("packages.restore"));
        connect(restore, &QAction::triggered, this, [this, application] {
            runFrontendOperation(application.id, QStringLiteral("restore_deployment"));
        });
    }
    moreMenu->addSeparator();
    auto* remove = moreMenu->addAction(t("apps.delete"));
    remove->setObjectName(QStringLiteral("deleteApplication:") + application.id);
    more->setMenu(moreMenu);
    layout->addWidget(more);

    const QString applicationId = application.id;
    connect(channelButton, &QPushButton::clicked, this, [this, applicationId]() { showApplicationEditor(applicationId, 1); });
    connect(settings, &QPushButton::clicked, this, [this, applicationId]() { showApplicationEditor(applicationId); });
    connect(launch, &QPushButton::clicked, this, [this, applicationId]() {
        launchProfile(applicationId);
    });
    connect(native, &QAction::triggered, this, [this, applicationId]() { executeProfileLaunch(applicationId, true); });
    connect(shortcut, &QAction::triggered, this, [this, applicationId]() { createProfileShortcutAction(applicationId); });
    connect(location, &QAction::triggered, this, [this, application]() {
        const SoftwareInstallation* software =
            model_.softwareById(application.softwareInstallationId);
        if (software == nullptr || !QFileInfo(software->executablePath).isFile()) {
            QMessageBox::warning(this, t("apps.menu.location"),
                                 t("common.softwareUnavailable"));
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QFileInfo(software->executablePath).absolutePath()));
    });
    connect(currentProcess, &QAction::triggered, this, [this, applicationId]() {
        showActiveLaunch(applicationId);
    });
    connect(stopProfile, &QAction::triggered, this, [this, applicationId]() {
        stopActiveLaunch(applicationId);
    });
    connect(probe, &QAction::triggered, this, [this, applicationId, application]() {
        const SoftwareInstallation* installation =
            model_.softwareById(application.softwareInstallationId);
        if (installation != nullptr &&
            (installation->source == QStringLiteral("Manual") ||
             installation->source == QStringLiteral("手动添加"))) {
            QString error;
            if (!model_.reinspectManualSoftware(installation->id, &error) && !error.isEmpty()) {
                QMessageBox::warning(this, t("nav.software"), error);
                return;
            }
            saveWorkspace(false);
            rebuildSoftwarePage();
            rebuildApplicationsList();
        }
        executeProfileLaunch(applicationId, false, true);
    });
    if (deployment != nullptr) {
        connect(deployment, &QAction::triggered, this, [this, applicationId, pluginDeployment]() {
            if (pluginDeployment) runFrontendOperation(applicationId, QStringLiteral("deploy"));
            else showDeveloperDeploymentDialog(applicationId);
        });
    }
    connect(duplicate, &QAction::triggered, this, [this, applicationId]() {
        const ApplicationProfile* source = model_.applicationById(applicationId);
        if (source == nullptr) return;
        const ApplicationProfile application = *source;
        QString created;
        if (model_.addApplication(application.softwareInstallationId, application.name + t("apps.copySuffix"), &created)) {
            if (ApplicationProfile* copy = model_.mutableApplicationById(created)) {
                copy->integrationAdapterId = application.integrationAdapterId;
                copy->integrationName = application.integrationName;
                copy->assignmentMode = application.assignmentMode;
                copy->channelCount = application.channelCount;
                copy->mappings = application.mappings;
                copy->configurationPath = application.configurationPath;
                copy->commandLineArguments = application.commandLineArguments;
            }
            saveWorkspace();
            rebuildApplicationsList();
            rebuildHardwarePage();
            rebuildSoftwarePage();
        }
    });
    connect(remove, &QAction::triggered, this, [this, applicationId, application]() {
        if (isProfileActive(applicationId)) {
            QMessageBox::warning(this, t("apps.delete"),
                                 t("apps.deleteRunning"));
            return;
        }
        if (QMessageBox::question(this, t("apps.delete"), t("apps.deleteQuestion").arg(application.name))
            == QMessageBox::Yes) {
            model_.removeApplication(applicationId);
            saveWorkspace();
            rebuildApplicationsList();
            rebuildHardwarePage();
            rebuildSoftwarePage();
        }
    });
    return card;
}

void MainWindow::rebuildApplicationsList() {
    if (applicationsList_ == nullptr) return;
    if (auto* subtitle = findChild<QLabel*>(QStringLiteral("applicationInventorySummary"))) {
        int online = 0;
        for (const Endpoint& endpoint : model_.endpoints())
            if (isCanEndpoint(endpoint) && endpoint.state == EndpointState::Online) ++online;
        if (isProduction()) subtitle->setText(t("apps.subtitle.production")
            .arg(model_.applications().size()));
    }
    clearLayout(applicationsList_);
    const Project* project = model_.projectById(model_.activeProjectId());
    const auto applications = model_.applicationsForProject(model_.activeProjectId());
    if (projectSummary_ != nullptr) {
        const QString projectName = project == nullptr ? t("project.default") : projectDisplayName(*project);
        projectSummary_->setText(QStringLiteral("%1 · %2")
                                     .arg(projectName, t("project.appCount").arg(applications.size())));
    }
    if (applications.isEmpty()) {
        auto* guide = cardFrame();
        guide->setObjectName(QStringLiteral("applicationQuickStart"));
        auto* guideLayout = new QVBoxLayout(guide);
        guideLayout->setContentsMargins(24, 22, 24, 22);
        guideLayout->setSpacing(15);
        guideLayout->addWidget(sectionTitle(t(model_.applications().isEmpty()
            ? "apps.empty.title" : "apps.empty.project"), guide));
        guideLayout->addWidget(mutedText(t("apps.empty.help"), guide));
        const char* titles[] = {"apps.empty.hardware", "apps.empty.software", "apps.empty.create"};
        const char* details[] = {"apps.empty.hardwareHelp", "apps.empty.softwareHelp", "apps.empty.createHelp"};
        for (int step = 0; step < 3; ++step) {
            auto* row = new QHBoxLayout();
            row->setSpacing(16);
            auto* number = new QLabel(QString::number(step + 1), guide);
            number->setObjectName(QStringLiteral("letterBadge"));
            number->setAlignment(Qt::AlignCenter);
            row->addWidget(number);
            auto* content = new QVBoxLayout();
            content->addWidget(sectionTitle(t(titles[step]), guide));
            content->addWidget(mutedText(t(details[step]), guide));
            row->addLayout(content, 1);
            if (step < 2) {
                auto* visit = new QPushButton(t(step == 0 ? "nav.hardware" : "nav.software"), guide);
                visit->setObjectName(QStringLiteral("secondaryButton"));
                row->addWidget(visit);
                connect(visit, &QPushButton::clicked, this, [this, step] { selectPage(step == 0 ? 1 : 2); });
            }
            guideLayout->addLayout(row);
        }
        applicationsList_->addWidget(guide);
    }
    for (const ApplicationProfile& application : applications) applicationsList_->addWidget(createApplicationCard(application));
    auto* add = new QPushButton(t("apps.addToProject"));
    add->setObjectName(QStringLiteral("addCard"));
    add->setProperty("automationId", QStringLiteral("addApplicationToProject"));
    connect(add, &QPushButton::clicked, this, &MainWindow::showAddApplicationDialog);
    applicationsList_->addWidget(add);
    applicationsList_->addStretch();
    updateStatusSummary();
}

QWidget* MainWindow::createHardwarePage() {
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(34, 28, 34, 26);
    pageLayout->setSpacing(16);
    auto* scan = new QPushButton(t("hardware.rescan"), page);
    scan->setObjectName(QStringLiteral("primaryButton"));
    auto* toolbar = new QWidget(page);
    auto* tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(0, 0, 0, 0);
    auto* filter = new QLineEdit(toolbar);
    filter->setObjectName(QStringLiteral("hardwareFilter"));
    filter->setPlaceholderText(t("hardware.filterPlaceholder"));
    filter->setAccessibleName(t("hardware.filterName"));
    filter->setClearButtonEnabled(true);
    filter->setMinimumWidth(260);
    filter->setMaximumWidth(360);
    filter->setText(hardwareFilter_);
    tools->addWidget(filter);
    tools->addWidget(scan);
    int knownCanEndpoints = 0;
    for (const Endpoint& endpoint : model_.endpoints()) {
        if (isCanEndpoint(endpoint) && !endpoint.historicalBinding && model_.backendEnabledForEndpoint(endpoint)) ++knownCanEndpoints;
    }
    QString subtitle = t(usesProductCopy() ? "hardware.subtitle.real" : "hardware.subtitle.explain");
    if (isProduction()) {
        if (!brokerInventoryFresh_) {
            subtitle = t("hardware.productionCached")
                           .arg(knownCanEndpoints);
        } else {
            QSet<QString> adapters;
            int online = 0;
            for (const Endpoint& endpoint : model_.endpoints()) {
                if (!isCanEndpoint(endpoint) || endpoint.historicalBinding || !model_.backendEnabledForEndpoint(endpoint)) continue;
                adapters.insert(endpoint.adapterId);
                if (endpoint.state == EndpointState::Online) ++online;
            }
            subtitle = t("hardware.productionFresh")
                           .arg(adapters.size()).arg(model_.fullAccessMappings().size()).arg(online);
        }
    }
    pageLayout->addWidget(createPageHeader(t("hardware.eyebrow"), t("nav.hardware"),
                                           subtitle, toolbar));

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(11);
    auto* noMatches = mutedText(t("hardware.filterEmpty"), content);
    noMatches->setObjectName(QStringLiteral("hardwareFilterEmpty"));
    layout->addWidget(noMatches);
    QMap<QString, QVector<Endpoint>> groups;
    for (const Endpoint& endpoint : model_.endpoints()) {
        if (isCanEndpoint(endpoint) && !endpoint.historicalBinding && model_.backendEnabledForEndpoint(endpoint)) groups[endpoint.adapterId].push_back(endpoint);
    }
    if (groups.isEmpty()) {
        auto* empty = cardFrame(content);
        auto* emptyLayout = new QVBoxLayout(empty);
        emptyLayout->setContentsMargins(18, 18, 18, 18);
        emptyLayout->addWidget(sectionTitle(t("hardware.noEndpoints"), empty));
        emptyLayout->addWidget(mutedText(
            t("hardware.noEndpointsHelp"),
            empty));
        layout->addWidget(empty);
    }
    for (auto group = groups.cbegin(); group != groups.cend(); ++group) {
        const QVector<Endpoint>& endpoints = group.value();
        if (endpoints.isEmpty()) continue;
        const Endpoint& first = endpoints.first();
        auto* card = cardFrame(content);
        card->setProperty("hardwareAdapter", true);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(17, 15, 17, 15);
        cardLayout->setSpacing(8);
        auto* header = new QWidget(card);
        auto* headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        auto* title = new QLabel(adapterDisplay(first), header);
        title->setObjectName(QStringLiteral("cardTitle"));
        headerLayout->addWidget(title);
        headerLayout->addWidget(badgeLabel(model_.backendDisplayName(first), QStringLiteral("info"), header));
        headerLayout->addStretch();
        QStringList identity;
        if (!maskedSerial(first.serial).isEmpty())
            identity << QStringLiteral("SN %1").arg(maskedSerial(first.serial));
        if (!first.firmwareVersion.isEmpty() && !first.firmwareVersion.startsWith(QStringLiteral("Not reported")))
            identity << first.firmwareVersion;
        identity << t("apps.channelCount").arg(endpoints.size());
        auto* identityLabel = new QLabel(identity.join(QStringLiteral(" · ")), header);
        identityLabel->setObjectName(QStringLiteral("muted"));
        identityLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        identityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        headerLayout->addWidget(identityLabel);
        cardLayout->addWidget(header);
        for (const Endpoint& endpoint : endpoints) {
            auto* row = new QFrame(card);
            row->setObjectName(QStringLiteral("softPanel"));
            row->setProperty("hardwareEndpointId", endpoint.id);
            row->setProperty("hardwareSearchText", QStringList{
                model_.backendDisplayName(endpoint), endpoint.backendFamily, endpoint.backendInstanceId,
                endpoint.vendor, endpoint.model, endpoint.serial,
                QStringLiteral("%1 %2 %1%2").arg(endpoint.busType).arg(endpoint.physicalChannel)
            }.join(QLatin1Char(' ')));
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(12, 9, 12, 9);
            rowLayout->addWidget(sectionTitle(QStringLiteral("%1 %2").arg(endpoint.busType).arg(endpoint.physicalChannel), row));
            const QString stateText = isProduction() && !brokerInventoryFresh_
                ? endpointStateDisplay(EndpointState::Offline)
                : endpoint.compatibilityIssue == QStringLiteral("iso-can-fd-required")
                    ? t("hardware.canFdRequired") : endpointStateDisplay(endpoint.state);
            const QString kind = isProduction() && !brokerInventoryFresh_
                ? QStringLiteral("muted") : stateKind(endpoint.state);
            rowLayout->addWidget(badgeLabel(stateText, kind, row));
            const QString allCapabilities = metadataDisplay(canOnlyCapabilities(endpoint.capabilities));
            QStringList protocols;
            for (const QString& protocol : {QStringLiteral("CAN"), QStringLiteral("CAN FD"), QStringLiteral("BRS")})
                if (allCapabilities.split(QStringLiteral(" / ")).contains(protocol)) protocols << protocol;
            auto* capabilities = mutedText(protocols.join(QStringLiteral(" / ")), row);
            capabilities->setToolTip(allCapabilities);
            rowLayout->addWidget(capabilities, 1);
            int visibleCount = 0;
            for (const ApplicationProfile& application : model_.applications()) {
                const auto mappings = model_.effectiveMappingsForApplication(application.id);
                const bool contains = std::any_of(mappings.cbegin(), mappings.cend(),
                                                  [&endpoint](const ChannelMapping& mapping) { return mapping.endpointId == endpoint.id; });
                if (contains) ++visibleCount;
            }
            auto* assign = new QPushButton(t("hardware.visibleApps").arg(visibleCount), row);
            assign->setObjectName(visibleCount > 0 ? QStringLiteral("assignedButton") : QStringLiteral("secondaryButton"));
            if (endpoint.state == EndpointState::Incompatible) {
                assign->setEnabled(false);
                assign->setToolTip(endpoint.compatibilityIssue == QStringLiteral("iso-can-fd-required")
                    ? t("hardware.canFdRequired") : endpointStateDisplay(endpoint.state));
            }
            assign->setEnabled(visibleCount > 0);
            rowLayout->addWidget(assign);
            connect(assign, &QPushButton::clicked, this, [this, endpoint]() { showHardwareAssignmentDialog(endpoint.id); });
            cardLayout->addWidget(row);
        }
        layout->addWidget(card);
    }
    layout->addStretch();
    scroll->setWidget(content);
    pageLayout->addWidget(scroll, 1);
    const auto applyFilter = [this, page, noMatches](const QString& text) {
        hardwareFilter_ = text;
        const auto words = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        int matches = 0, total = 0;
        for (auto* card : page->findChildren<QFrame*>()) {
            if (!card->property("hardwareAdapter").toBool()) continue;
            bool any = false;
            for (auto* row : card->findChildren<QFrame*>()) {
                if (row->property("hardwareEndpointId").toString().isEmpty()) continue;
                ++total;
                const auto label = row->property("hardwareSearchText").toString();
                const bool match = std::all_of(words.cbegin(), words.cend(), [&](const auto& word) { return label.contains(word, Qt::CaseInsensitive); });
                row->setVisible(match);
                any = any || match;
                if (match) ++matches;
            }
            card->setVisible(any);
        }
        noMatches->setVisible(total > 0 && matches == 0);
    };
    connect(filter, &QLineEdit::textChanged, page, applyFilter);
    applyFilter(filter->text());
    connect(scan, &QPushButton::clicked, this, [this]() {
        if (isProduction()) refreshRuntimeInventory(true);
        else refreshInventoryAction(QStringLiteral("所有已启用后端和硬件通道"));
    });
    return page;
}

void MainWindow::rebuildHardwarePage() {
    if (pages_ == nullptr || hardwarePage_ == nullptr) return;
    const int currentIndex = pages_->currentIndex();
    QWidget* previous = hardwarePage_;
    hardwarePage_ = createHardwarePage();
    pages_->removeWidget(previous);
    pages_->insertWidget(1, hardwarePage_);
    previous->deleteLater();
    pages_->setCurrentIndex(currentIndex);
}

void MainWindow::rebuildSoftwarePage() {
    if (pages_ == nullptr || softwarePage_ == nullptr) return;
    const int currentIndex = pages_->currentIndex();
    QWidget* previous = softwarePage_;
    softwarePage_ = createSoftwarePage();
    pages_->removeWidget(previous);
    pages_->insertWidget(2, softwarePage_);
    previous->deleteLater();
    pages_->setCurrentIndex(currentIndex);
}

void MainWindow::rebuildBackendsPage() {
    if (pages_ == nullptr || backendsPage_ == nullptr) return;
    const int currentIndex = pages_->currentIndex();
    QWidget* previous = backendsPage_;
    backendsPage_ = createBackendsPage();
    pages_->removeWidget(previous);
    pages_->insertWidget(3, backendsPage_);
    previous->deleteLater();
    pages_->setCurrentIndex(currentIndex);
}

void MainWindow::refreshRuntimeInventory(bool notifyOnError, bool scanSoftware) {
    if (!isProduction()) return;
    if (discoveryWatcher_ != nullptr) {
        inventoryRefreshPending_ = true;
        inventorySoftwarePending_ |= scanSoftware;
        inventoryNotifyPending_ |= notifyOnError;
        if (notifyOnError) updateStatusSummary(t("status.scanInProgress"));
        return;
    }
    scanSoftware |= inventorySoftwarePending_;
    notifyOnError |= inventoryNotifyPending_;
    inventoryRefreshPending_ = inventorySoftwarePending_ = inventoryNotifyPending_ = false;

    updateStatusSummary(t("status.connectingBroker"));
    const auto generation = backendCatalogGeneration_;
    const auto topology = hardwareChangeMonitor_ == nullptr ? 0 : hardwareChangeMonitor_->generation();
    auto* watcher = new QFutureWatcher<RuntimeDiscoveryResult>(this);
    discoveryWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<RuntimeDiscoveryResult>::finished, this,
            [this, watcher, notifyOnError, generation, topology, scanSoftware]() {
        const auto obsolete = [this, generation, topology] {
            return generation != backendCatalogGeneration_ ||
                (hardwareChangeMonitor_ != nullptr && topology != hardwareChangeMonitor_->generation());
        };
        const auto finish = [&, this] {
            if (discoveryWatcher_ == watcher) discoveryWatcher_ = nullptr;
            watcher->deleteLater();
            if (obsolete()) {
                brokerInventoryFresh_ = false;
                inventoryRefreshPending_ = true;
                inventorySoftwarePending_ |= scanSoftware;
            }
            if (inventoryRefreshPending_) {
                QTimer::singleShot(0, this, [this] {
                    // Preserve queued requests while a burst is still settling.
                    if (hardwareChangeMonitor_ != nullptr && hardwareChangeMonitor_->isDebouncing() &&
                        !inventoryNotifyPending_) return;
                    refreshRuntimeInventory(false, false);
                });
            }
        };
        const RuntimeDiscoveryResult result = watcher->result();
        if (obsolete()) {
            finish();
            return;
        }

        backendHealth_ = result.backendHealth;
        backendRuntimes_ = result.backendRuntimes;
        if (result.softwareInventoryScanned) {
            model_.reconcileSoftwareInstallations(result.softwareInstallations);
            rebuildSoftwarePage();
        }
        rebuildApplicationsList();
        if (notifyOnError && !result.softwareDiagnostics.isEmpty())
            QMessageBox::warning(this, t("nav.software"), result.softwareDiagnostics.join(QLatin1Char('\n')));
        if (obsolete()) { finish(); return; }

        if (!result.brokerAvailable || !result.error.isEmpty()) {
            const QString message = result.error.isEmpty()
                ? t("status.brokerUnavailable") : result.error;
            brokerInventoryFresh_ = false;
            saveWorkspace(false);
            rebuildHardwarePage();
            updateStatusSummary(t("status.mappingsPreserved").arg(message));
            rebuildBackendsPage();
            if (notifyOnError) {
                QMessageBox warning(QMessageBox::Warning, QStringLiteral("UCAL"),
                    t("status.reconnectHelp"), QMessageBox::Ok, this);
                warning.setDetailedText(message);
                warning.exec();
            }
            finish();
            runPendingProfileLaunches();
            return;
        }

        brokerInventoryFresh_ = true;
        QVector<Endpoint> packagedEndpoints;
        for (auto endpoint : result.endpoints) {
            const bool packaged = std::any_of(model_.backendPackages().cbegin(), model_.backendPackages().cend(),
                [&endpoint](const BackendPackage& package) { return endpoint.backendInstanceId.isEmpty()?package.mode.isEmpty()&&package.backendFamily==endpoint.backendFamily:package.id==endpoint.backendInstanceId; });
            if (packaged && model_.backendEnabledForEndpoint(endpoint)) {
                if(!endpoint.backendInstanceId.isEmpty()){
                    const auto* package=model_.backendPackageById(endpoint.backendInstanceId);
                    if(package){endpoint.backendName=package->name;endpoint.vendor=package->vendor;}
                }
                packagedEndpoints << endpoint;
            }
        }
        model_.reconcileInventory(packagedEndpoints);
        rebuildApplicationsList();
        saveWorkspace(false);
        rebuildHardwarePage();
        rebuildBackendsPage();
        updateStatusSummary(packagedEndpoints.isEmpty()
            ? t("status.brokerOnlineEmpty")
            : t("status.inventoryRefreshed").arg(packagedEndpoints.size()));
        QStringList warnings;
        for (const auto& warning : result.backendWarnings) {
            bool disabled = false;
            for (const auto& id : preferences_.disabledBackendIds)
                if (warning.startsWith(id + QStringLiteral(" backend:")) || warning.startsWith(id + QLatin1Char(':'))) disabled = true;
            if (!disabled) warnings << warning;
        }
        if (!warnings.isEmpty()) updateStatusSummary(t("status.partialBackendFailure").arg(warnings.join(QStringLiteral("; "))));
        finish();
        runPendingProfileLaunches();
    });
    watcher->setFuture(QtConcurrent::run([scanSoftware]() {
        startBrokerIfNeeded();
        auto result = discoverRuntimeEndpoints(scanSoftware);
        return result;
    }));
}

void MainWindow::rescanSoftwareInstallations() {
    if (!isProduction()) { updateStatusSummary(t("status.scanComplete")); return; }
    if (softwareDiscoveryWatcher_ != nullptr) { updateStatusSummary(t("software.scanRunning")); return; }
    updateStatusSummary(t("software.scanRunning"));
    auto* watcher = new QFutureWatcher<RuntimeDiscoveryResult>(this);
    softwareDiscoveryWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<RuntimeDiscoveryResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        softwareDiscoveryWatcher_ = nullptr; watcher->deleteLater();
        model_.reconcileSoftwareInstallations(result.softwareInstallations);
        saveWorkspace(false); rebuildSoftwarePage(); rebuildApplicationsList();
        updateStatusSummary(t("software.scanComplete").arg(result.softwareInstallations.size()));
        if (!result.softwareDiagnostics.isEmpty())
            QMessageBox::warning(this, t("nav.software"), result.softwareDiagnostics.join(QLatin1Char('\n')));
    });
    watcher->setFuture(QtConcurrent::run([] {
        RuntimeDiscoveryResult result;
        result.softwareInstallations = discoverInstalledSoftware(&result.softwareDiagnostics);
        return result;
    }));
}

QWidget* MainWindow::createSoftwarePage() {
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(34, 28, 34, 26);
    pageLayout->setSpacing(16);
    auto* actions = new QWidget(page);
    auto* actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    auto* scan = new QPushButton(t("apps.scan"), actions);
    scan->setProperty("automationId", QStringLiteral("rescanSoftware"));
    scan->setObjectName(QStringLiteral("secondaryButton"));
    auto* add = new QPushButton(t("software.addExe"), actions);
    add->setObjectName(QStringLiteral("primaryButton"));
    actionsLayout->addWidget(scan);
    actionsLayout->addWidget(add);
    const QString softwareSubtitle = isProduction()
        ? t("software.subtitle.production").arg(model_.softwareInstallations().size())
        : t(isRealistic() ? "software.subtitle.real" : "software.subtitle.explain");
    pageLayout->addWidget(createPageHeader(t("software.eyebrow"), t("nav.software"),
                                           softwareSubtitle, actions));
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(9);
    for (const SoftwareInstallation& software : model_.softwareInstallations()) {
        auto* card = cardFrame(content);
        auto* row = new QHBoxLayout(card);
        row->setContentsMargins(17, 14, 17, 14);
        auto* icon = new QLabel(software.displayName.left(1), card);
        icon->setObjectName(QStringLiteral("letterBadge"));
        icon->setAlignment(Qt::AlignCenter);
        row->addWidget(icon);
        auto* text = new QWidget(card);
        auto* textLayout = new QVBoxLayout(text);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(3);
        auto* title = new QLabel(QStringLiteral("%1 %2 · %3").arg(software.displayName, metadataDisplay(software.version), software.architecture), text);
        title->setObjectName(QStringLiteral("cardTitle"));
        textLayout->addWidget(title);
        textLayout->addWidget(mutedText(software.executablePath, text));
        textLayout->addWidget(mutedText(
            softwareCompatibilityDisplay(software), text));
        row->addWidget(text, 1);
        if (!software.available)
            row->addWidget(badgeLabel(t("software.compat.missing"), QStringLiteral("warning"), card));
        const int applicationCount = model_.applicationNamesUsingSoftware(software.id).size();
        auto* applications = new QPushButton(t("software.applicationCount").arg(applicationCount), card);
        applications->setObjectName(QStringLiteral("ghostButton"));
        applications->setProperty("softwareApplications", software.id);
        applications->setEnabled(applicationCount > 0);
        row->addWidget(applications);
        connect(applications, &QPushButton::clicked, this, [this, software] { showSoftwareApplications(software.id); });
        auto* create = new QPushButton(t("software.createApp"), card);
        create->setObjectName(QStringLiteral("secondaryButton"));
        create->setProperty("softwareId", software.id);
        create->setEnabled(software.available && software.compatibilityKind != QStringLiteral("danger") &&
            std::any_of(software.integrationAdapters.cbegin(), software.integrationAdapters.cend(),
                        [](const QString& id) { return id != QStringLiteral("native"); }) &&
            frontendPackage(software.family).operations.contains(QStringLiteral("prepare_launch")));
        if (!create->isEnabled()) create->setToolTip(softwareCompatibilityDisplay(software));
        row->addWidget(create);
        auto* more = new QToolButton(card);
        more->setText(QStringLiteral("•••"));
        more->setObjectName(QStringLiteral("ghostButton"));
        more->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(more);
        auto* rescanAction = menu->addAction(t("software.rescan"));
        auto* locationAction = menu->addAction(t("software.location"));
        const auto frontend = frontendPackage(software.family);
        if (frontend.configurationTool.available()) {
            auto* configure = menu->addAction(t("packages.configure"));
            connect(configure, &QAction::triggered, this, [this, frontend] {
                QString error;
                if (!startPackageTool(frontend.configurationTool, &error))
                    QMessageBox::warning(this, t("packages.configure"), error);
            });
        }
        QAction* forgetAction = nullptr;
        if (software.source == QStringLiteral("手动添加") || software.source == QStringLiteral("Manual")) {
            forgetAction = menu->addAction(t("software.forget"));
        }
        more->setMenu(menu);
        row->addWidget(more);
        connect(create, &QPushButton::clicked, this, [this, software]() {
            showApplicationEditor(QString(), 1, software.id);
        });
        connect(rescanAction, &QAction::triggered, this, [this, software]() {
            if (isProduction() &&
                (software.source == QStringLiteral("Manual") ||
                 software.source == QStringLiteral("手动添加"))) {
                QString error;
                if (!model_.reinspectManualSoftware(software.id, &error) && !error.isEmpty()) {
                    QMessageBox::warning(this, t("nav.software"), error);
                    return;
                }
                saveWorkspace(false);
                rebuildSoftwarePage();
                rebuildApplicationsList();
                updateStatusSummary(t("software.rescanComplete").arg(software.displayName));
            } else if (isProduction()) {
                rescanSoftwareInstallations();
            }
            else refreshInventoryAction(QStringLiteral("software inventory"));
        });
        connect(locationAction, &QAction::triggered, this, [this, software]() {
            if (!QFileInfo(software.executablePath).isFile()) {
                QMessageBox::warning(this, t("software.location"),
                                     t("common.softwareUnavailable"));
                return;
            }
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                QFileInfo(software.executablePath).absolutePath()));
        });
        if (forgetAction != nullptr) {
            connect(forgetAction, &QAction::triggered, this, [this, software]() {
                const QStringList references = model_.applicationNamesUsingSoftware(software.id);
                if (!references.isEmpty()) {
                    QMessageBox::warning(
                        this, t("software.forget"),
                        t("software.forget.used").arg(
                            references.join(QStringLiteral("\n"))));
                    return;
                }
                if (QMessageBox::question(this, t("software.forget"),
                                          t("software.forget.question")) !=
                    QMessageBox::Yes) {
                    return;
                }
                if (model_.removeManualSoftware(software.id)) {
                    saveWorkspace();
                    rebuildSoftwarePage();
                }
            });
        }
        layout->addWidget(card);
    }
    layout->addStretch();
    scroll->setWidget(content);
    pageLayout->addWidget(scroll, 1);
    connect(scan, &QPushButton::clicked, this, &MainWindow::rescanSoftwareInstallations);
    connect(add, &QPushButton::clicked, this, [this]() {
        const QString exe = QFileDialog::getOpenFileName(this, t("addApp.selectExe"),
            QString(), t("addApp.exeFilter"));
        if (exe.isEmpty()) return;
        QString error;
        if (model_.addManualSoftware(exe, &error).isEmpty()) {
            if (!error.isEmpty()) QMessageBox::warning(this, t("nav.software"), error);
            return;
        }
        saveWorkspace();
        rebuildSoftwarePage();
        rebuildApplicationsList();
    });
    return page;
}

void MainWindow::showSoftwareApplications(const QString& softwareId) {
    const auto* software = model_.softwareById(softwareId);
    if (software == nullptr) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("softwareApplicationsDialog"));
    dialog.setWindowTitle(t("software.dependencies"));
    dialog.resize(610, 440);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->addWidget(sectionTitle(software->displayName + QStringLiteral(" ") + software->version, &dialog));
    layout->addWidget(mutedText(t("software.applicationHelp"), &dialog));
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("softwareApplicationList"));
    for (const auto& application : model_.applications()) {
        if (application.softwareInstallationId != softwareId) continue;
        auto* item = new QListWidgetItem(application.name, list);
        item->setData(Qt::UserRole, application.id);
        item->setSizeHint(QSize(0, 42));
    }
    layout->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* settings = buttons->addButton(t("common.settings"), QDialogButtonBox::ActionRole);
    settings->setEnabled(false);
    QString chosen;
    connect(list, &QListWidget::currentRowChanged, &dialog, [settings](int row) { settings->setEnabled(row >= 0); });
    connect(settings, &QPushButton::clicked, &dialog, [&] {
        if (list->currentItem() == nullptr) return;
        chosen = list->currentItem()->data(Qt::UserRole).toString();
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
    if (!chosen.isEmpty()) showApplicationEditor(chosen);
}

void MainWindow::rescanBackendPackages() {
    ++backendCatalogGeneration_;
    brokerInventoryFresh_ = false;
    auto scan = scanBackendDirectory(backendDirectory(QCoreApplication::applicationDirPath()));
    for (auto& package : scan.packages) package.enabled = !preferences_.disabledBackendIds.contains(package.id);
    const auto pluginErrors=synchronizeBackendPlugins(scan.packages,QCoreApplication::applicationDirPath());
    if(!pluginErrors.isEmpty())scan.diagnostics<<pluginErrors;
    model_.reconcileBackendPackages(scan.packages);
    const auto knownPackages = model_.backendPackages();
    for (const auto& package : knownPackages) model_.setBackendPackageEnabled(package.id, !preferences_.disabledBackendIds.contains(package.id));
    rebuildApplicationsList();
    rebuildHardwarePage();
    const QString logs = QDir(QFileInfo(workspacePath_).absolutePath()).filePath(QStringLiteral("run-logs"));
    QDir().mkpath(logs);
    saveOperationEvidence(QDir(logs).filePath(QStringLiteral("backend-scan.log")),
                          scan.diagnostics.join(QLatin1Char('\n')).toUtf8());
    rebuildBackendsPage();
    updateStatusSummary(t("backends.scanComplete").arg(model_.backendPackages().size()));
    if (!scan.diagnostics.isEmpty()) QMessageBox::warning(this, t("nav.backends"), scan.diagnostics.join(QLatin1Char('\n')));
    if (runtimeActionsEnabled_) refreshRuntimeInventory(true);
}

void MainWindow::toggleBackendPackage(const QString& packageId) {
    const auto* package = model_.backendPackageById(packageId);
    if (package == nullptr) return;
    auto changed = preferences_;
    if (package->enabled) changed.disabledBackendIds.insert(packageId);
    else changed.disabledBackendIds.remove(packageId);
    if (!saveUserPreferences(changed)) {
        QMessageBox::warning(this, t("nav.backends"), t("backends.saveFailed"));
        return;
    }
    preferences_ = changed;
    rescanBackendPackages();
}

QString MainWindow::backendStatus(const BackendPackage& package) const {
    if (!package.enabled) return t("common.disabled");
    if (!package.manifestAvailable) return t("backends.packageMissing");
    if (!package.loadError.isEmpty()) return package.loadError;
    if (package.mode == QStringLiteral("j2534") && !package.runtimeDiscovery) {
        QString error;
        if (loadBackendPluginChannels(package, &error).isEmpty() && error.isEmpty()) return t("plugins.noChannels");
    }
    if (!brokerInventoryFresh_) return t("backends.notChecked");
    for (const QString& health : backendHealth_) {
        if (health.section(QLatin1Char(':'), 0, 0).compare(package.id, Qt::CaseInsensitive) != 0) continue;
        if (health.contains(QStringLiteral(": unsupported ("))) return t("backends.capabilityMismatch");
        if (health.contains(QStringLiteral(": incompatible version ("))) return t("backends.runtimeIncompatible");
        return t(health.contains(QStringLiteral(": ok (")) ? "backends.available" : "backends.runtimeMissing");
    }
    return t("backends.notActive");
}

QWidget* MainWindow::createBackendsPage() {
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(34, 28, 34, 26);
    pageLayout->setSpacing(16);
    auto* scan = new QPushButton(t("hardware.rescan"), page);
    scan->setObjectName(QStringLiteral("primaryButton"));
    scan->setProperty("automationId", QStringLiteral("rescanBackends"));
    connect(scan, &QPushButton::clicked, this, &MainWindow::rescanBackendPackages);
    pageLayout->addWidget(createPageHeader(t("backends.eyebrow"), t("nav.backends"),
        t("backends.folderSummary").arg(model_.backendPackages().size()), scan));
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(12);
    for (const BackendPackage& package : model_.backendPackages()) {
        auto* card = cardFrame(content);
        auto* row = new QHBoxLayout(card);
        row->setContentsMargins(20, 20, 20, 20);
        auto* text = new QVBoxLayout();
        text->addWidget(sectionTitle(package.name, card));
        text->addWidget(mutedText(metadataDisplay(canOnlyCapabilities(package.capabilities)), card));
        int count = 0;
        for (const auto& endpoint : model_.endpoints())
            if ((package.mode.isEmpty() ? (endpoint.backendInstanceId.isEmpty() && endpoint.backendFamily == package.backendFamily) : endpoint.backendInstanceId == package.id) && isCanEndpoint(endpoint) &&
                (endpoint.state == EndpointState::Online || endpoint.state == EndpointState::Busy)) ++count;
        text->addWidget(mutedText(t("backends.hardwareCount").arg(count), card));
        row->addLayout(text, 1);
        const QString state = backendStatus(package);
        if (package.enabled)
            row->addWidget(badgeLabel(state, state == t("backends.available") ? QStringLiteral("success") : QStringLiteral("muted"), card));
        const auto stateText = t(package.enabled ? "backends.enabledState" : "common.disabled");
        const auto actionText = t(package.enabled ? "backends.disable" : "backends.enable");
        auto* toggle = new QPushButton(stateText, card);
        toggle->setObjectName(package.enabled ? QStringLiteral("backendEnabledButton") : QStringLiteral("backendDisabledButton"));
        toggle->setProperty("backendToggleId", package.id);
        toggle->setAccessibleName(QStringLiteral("%1: %2 (%3)").arg(package.name, stateText, actionText));
        toggle->setToolTip(actionText + QLatin1Char('\n') + t("backends.toggleHelp"));
        row->addWidget(toggle);
        connect(toggle, &QPushButton::clicked, this, [this, id = package.id] { toggleBackendPackage(id); });
        auto* details = new QPushButton(t("common.details"), card);
        details->setObjectName(QStringLiteral("secondaryButton"));
        details->setProperty("backendId", package.id);
        row->addWidget(details);
        connect(details, &QPushButton::clicked, this, [this, package]() { showBackendPackageManager(package.id); });
        layout->addWidget(card);
    }
    layout->addStretch();
    scroll->setWidget(content);
    pageLayout->addWidget(scroll, 1);
    return page;
}

void MainWindow::showBackendPackageManager(const QString& packageId) {
    const BackendPackage* found = model_.backendPackageById(packageId);
    if (found == nullptr) return;
    const BackendPackage package = *found;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("backendPackageDetails"));
    dialog.setWindowTitle(package.name + QStringLiteral(" · ") + t("common.details"));
    dialog.resize(640, 370);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->addWidget(sectionTitle(package.name, &dialog));
    auto* form = new QFormLayout();
    form->setVerticalSpacing(14);
    form->addRow(t("backends.vendor"), new QLabel(package.vendor, &dialog));
    form->addRow(t("backends.version"), new QLabel(package.version, &dialog));
    form->addRow(t("backends.status"), new QLabel(backendStatus(package), &dialog));
    auto* path = mutedText(package.manifestPath.isEmpty() ? t("metadata.notReported")
        : QDir::toNativeSeparators(QFileInfo(package.manifestPath).absolutePath()), &dialog);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(t("backends.folder"), path);
    auto addFile = [&](const QString& caption, const QString& file) {
        auto* label = mutedText(file.isEmpty() ? t("metadata.notReported") : QDir::toNativeSeparators(file), &dialog);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(caption, label);
    };
    form->addRow(t("backends.templateType"), new QLabel(package.type, &dialog));
    addFile(t("backends.library"), package.libraryPath);
    form->addRow(t("backends.libraryVersion"), new QLabel(runtimeFileVersion(package.libraryPath).isEmpty()
        ? t("metadata.notReported") : runtimeFileVersion(package.libraryPath), &dialog));
    if (package.configurationTool.available()) addFile(t("packages.configurationProgram"), package.configurationTool.program);

    const auto loaded = backendRuntimes_.value(package.mode.isEmpty() ? package.backendFamily.toLower() : package.id);
    if (!loaded.fileVersion.isEmpty())
        form->addRow(t("backends.runtimeSelectedVersion"), new QLabel(loaded.fileVersion, &dialog));
    layout->addLayout(form);
    layout->addWidget(sectionTitle(t("backends.capabilities"), &dialog));
    layout->addWidget(mutedText(metadataDisplay(canOnlyCapabilities(package.capabilities)), &dialog));
    if (!package.manifestAvailable) layout->addWidget(mutedText(t("backends.missingHelp"), &dialog));
    layout->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    if (package.configurationTool.available() && package.manifestAvailable) {
        auto* configure = buttons->addButton(t("packages.configure"), QDialogButtonBox::ActionRole);
        configure->setObjectName(QStringLiteral("openBackendConfigurationTool"));
        connect(configure, &QPushButton::clicked, &dialog, [this, package, &dialog] {
            QString error;
            if (!startPackageTool(package.configurationTool, &error))
                QMessageBox::warning(&dialog, t("packages.configure"), error);
        });
    }
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

QWidget* MainWindow::createSettingsPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 26);
    layout->setSpacing(15);
    layout->addWidget(createPageHeader(t("settings.eyebrow"), t("nav.settings"),
                                        t(usesProductCopy() ? "settings.subtitle.real" : "settings.subtitle.explain")));
    auto* behavior = cardFrame(page);
    auto* behaviorLayout = new QGridLayout(behavior);
    behaviorLayout->setContentsMargins(18, 16, 18, 16);
    behaviorLayout->setHorizontalSpacing(22);
    behaviorLayout->setVerticalSpacing(10);
    behaviorLayout->addWidget(sectionTitle(t("settings.startup"), behavior), 0, 0, 1, 2);
    auto* autoStart = new QCheckBox(t("settings.autostart"), behavior);
    auto* startMinimized = new QCheckBox(t("settings.startMin"), behavior);
    auto* autoScan = mutedText(t("settings.autoScan"), behavior);
    auto* autoUpdate = new QCheckBox(t("settings.autoUpdate"), behavior);
    autoStart->setObjectName(QStringLiteral("autoStartPreference"));
    startMinimized->setObjectName(QStringLiteral("startMinimizedPreference"));

    autoStart->setChecked(isProduction() && preferences_.autoStart);
    startMinimized->setChecked(isProduction() && preferences_.startMinimized);

    autoUpdate->setEnabled(false);
    autoUpdate->setToolTip(t("settings.updateUnavailable"));
    behaviorLayout->addWidget(autoStart, 1, 0);
    behaviorLayout->addWidget(startMinimized, 2, 0);
    behaviorLayout->addWidget(autoScan, 1, 1);
    behaviorLayout->addWidget(autoUpdate, 2, 1);
    behaviorLayout->addWidget(new QLabel(t("settings.closeButton"), behavior), 3, 0);
    auto* closeBehavior = new QComboBox(behavior);
    closeBehavior->setObjectName(QStringLiteral("closeBehaviorPreference"));
    closeBehavior->addItems({t("settings.hideTray"), t("settings.ask"), t("settings.exit")});
    closeBehavior->setCurrentIndex(isProduction()
        ? static_cast<int>(preferences_.closeBehavior) : 0);
    behaviorLayout->addWidget(closeBehavior, 3, 1);
    layout->addWidget(behavior);

    auto* languageCard = cardFrame(page);
    auto* languageLayout = new QHBoxLayout(languageCard);
    languageLayout->setContentsMargins(18, 15, 18, 15);
    auto* languageText = new QWidget(languageCard);
    auto* languageTextLayout = new QVBoxLayout(languageText);
    languageTextLayout->setContentsMargins(0, 0, 0, 0);
    languageTextLayout->addWidget(sectionTitle(t("settings.language"), languageText));
    languageTextLayout->addWidget(mutedText(t("settings.languageHelp"), languageText));
    languageLayout->addWidget(languageText, 1);
    auto* languageCombo = new QComboBox(languageCard);
    const QVector<UiLanguage> languageOptions = {UiLanguage::System, UiLanguage::English,
                                                 UiLanguage::SimplifiedChinese, UiLanguage::German,
                                                 UiLanguage::Japanese};
    for (const UiLanguage option : languageOptions) {
        languageCombo->addItem(uiLanguageOptionText(option, language_), uiLanguageKey(option));
    }
    languageCombo->setCurrentIndex(languageOptions.indexOf(language_));
    languageCombo->setMinimumWidth(220);
    languageLayout->addWidget(languageCombo);
    layout->addWidget(languageCard);

    auto* about = cardFrame(page);
    auto* aboutLayout = new QHBoxLayout(about);
    aboutLayout->setContentsMargins(20, 18, 20, 18);
    auto* icon = new QLabel(about);
    icon->setObjectName(QStringLiteral("aboutLogo"));
    icon->setFixedSize(72, 72);
    icon->setPixmap(managerIcon().pixmap(72, 72));
    aboutLayout->addWidget(icon);
    auto* text = new QWidget(about);
    auto* textLayout = new QVBoxLayout(text);
    textLayout->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(usesProductCopy() ? QStringLiteral("UCAL Manager 0.9.1")
                                            : QStringLiteral("UCAL Manager 0.2.0-poc"), text);
    title->setObjectName(QStringLiteral("cardTitle"));
    textLayout->addWidget(title);
    auto* fullName = mutedText(QStringLiteral("Unified CAN Abstraction Layer"), text);
    fullName->setObjectName(QStringLiteral("aboutFullName"));
    textLayout->addWidget(fullName);
    textLayout->addWidget(mutedText(t("about.build").arg(QStringLiteral(__DATE__), QStringLiteral(__TIME__)), text));
    auto* link = new QLabel(QStringLiteral("<a href=\"ucal:project-license\">%1</a>")
                                .arg(t(usesProductCopy() ? "about.githubLicense" : "about.githubPoc")), text);
    link->setOpenExternalLinks(false);
    connect(link, &QLabel::linkActivated, this, [this](const QString&) {
        const QString root = resolveProductRoot(QCoreApplication::applicationDirPath());
        QFile text(firstExistingFile({QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("LICENSE")),
                                     QDir(root).filePath(QStringLiteral("LICENSE"))}));
        if (!text.open(QIODevice::ReadOnly)) {
            QMessageBox::information(this, t("about.githubLicense"), t("about.sourceHelp"));
            return;
        }
        QDialog license(this);
        license.setWindowTitle(t("about.githubLicense"));
        license.resize(720, 560);
        auto* layout = new QVBoxLayout(&license);
        auto* body = new QTextBrowser(&license);
        QString licenseText = QStringLiteral("UCAL public components: LGPL-3.0-only WITH Independent-modules-exception\n\n")
                            + QString::fromUtf8(text.readAll());
        const QDir licenseRoot(QFileInfo(text).absolutePath());
        for (const QString& relative : {QStringLiteral("LICENSE_SCOPE.md"),
                                       QStringLiteral("licenses/Independent-modules-exception.txt"),
                                       QStringLiteral("licenses/CompatibilitySupport.txt"),
                                       QStringLiteral("THIRD_PARTY_NOTICES.md")}) {
            QFile notice(licenseRoot.filePath(relative));
            if (notice.open(QIODevice::ReadOnly))
                licenseText += QStringLiteral("\n\n--- %1 ---\n\n").arg(relative)
                             + QString::fromUtf8(notice.readAll());
        }
        body->setPlainText(licenseText);
        layout->addWidget(body);
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &license);
        connect(close, &QDialogButtonBox::rejected, &license, &QDialog::reject);
        layout->addWidget(close);
        license.exec();
    });
    link->setTextInteractionFlags(Qt::TextBrowserInteraction);
    textLayout->addWidget(link);
    aboutLayout->addWidget(text, 1);
    auto* update = new QPushButton(t("settings.checkUpdate"), about);
    update->setObjectName(QStringLiteral("secondaryButton"));
    update->setEnabled(false);
    update->setToolTip(t("settings.updateSourceMissing"));
    aboutLayout->addWidget(update);


    auto* storage = cardFrame(page);
    auto* storageLayout = new QVBoxLayout(storage);
    storageLayout->setContentsMargins(18, 15, 18, 15);
    storageLayout->addWidget(sectionTitle(t("settings.localConfig"), storage));
    const QString storagePath = workspacePath_;
    storageLayout->addWidget(mutedText(storagePath, storage));
    storageLayout->addWidget(mutedText(t("settings.savedLocally"), storage));
    layout->addWidget(storage);
    layout->addWidget(about);
    layout->addStretch();

    connect(closeBehavior, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](const int index) {
        closeToTray_ = index == 0;
        askBeforeExit_ = index == 1;
        if (isProduction()) {
            preferences_.closeBehavior = static_cast<CloseBehavior>(index);
            savePreferences();
        }
    });
    connect(startMinimized, &QCheckBox::toggled, this,
            [this, autoStart](const bool checked) {
        if (!isProduction()) return;
        const bool previous = preferences_.startMinimized;
        preferences_.startMinimized = checked;
        QString error;
        if (preferences_.autoStart && !configureWindowsAutoStart(
                true, QCoreApplication::applicationFilePath(), checked, &error)) {
            preferences_.startMinimized = previous;
            QMessageBox::warning(this, t("settings.startup"), error);
            return;
        }
        savePreferences();
        autoStart->setChecked(preferences_.autoStart);
    });
    connect(autoStart, &QCheckBox::toggled, this,
            [this, autoStart](const bool checked) {
        if (!isProduction()) return;
        QString error;
        if (!configureWindowsAutoStart(checked, QCoreApplication::applicationFilePath(),
                                       preferences_.startMinimized, &error)) {
            const QSignalBlocker blocker(autoStart);
            autoStart->setChecked(!checked);
            QMessageBox::warning(this, t("settings.startup"), error);
            return;
        }
        preferences_.autoStart = checked;
        savePreferences();
    });
    languageCombo->setObjectName(QStringLiteral("uiLanguagePreference"));
    connect(languageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, languageOptions](const int index) {
        if (index < 0 || index >= languageOptions.size()) return;
        const UiLanguage selected = languageOptions.at(index);
        QTimer::singleShot(0, this, [this, selected]() { changeUiLanguage(selected); });
    });
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("settingsScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setWidget(page);
    return scroll;
}

void MainWindow::refreshProjectSelector() {
    if (projectSelector_ == nullptr) return;
    projectSelector_->blockSignals(true);
    projectSelector_->clear();
    int selected = 0;
    for (int index = 0; index < model_.projects().size(); ++index) {
        const Project& project = model_.projects().at(index);
        projectSelector_->addItem(projectDisplayName(project), project.id);
        if (project.id == model_.activeProjectId()) selected = index;
    }
    projectSelector_->setCurrentIndex(selected);
    projectSelector_->blockSignals(false);
}

void MainWindow::showProjectManager() {
    QDialog dialog(this);
    dialog.setWindowTitle(t("project.managerTitle"));
    dialog.resize(760, 480);
    auto* root = new QVBoxLayout(&dialog);
    root->addWidget(mutedText(t("project.managerHelp"), &dialog));
    auto* body = new QHBoxLayout();
    auto* projects = new QListWidget(&dialog);
    projects->setObjectName(QStringLiteral("projectList"));
    auto* applications = new QListWidget(&dialog);
    applications->setObjectName(QStringLiteral("projectApplicationList"));
    body->addWidget(projects, 1);
    body->addWidget(applications, 2);
    root->addLayout(body, 1);
    auto* actions = new QHBoxLayout();
    auto* add = new QPushButton(t("common.new"), &dialog);
    add->setObjectName(QStringLiteral("projectAddButton"));
    auto* rename = new QPushButton(t("common.rename"), &dialog);
    rename->setObjectName(QStringLiteral("projectRenameButton"));
    auto* remove = new QPushButton(t("common.delete"), &dialog);
    remove->setObjectName(QStringLiteral("projectDeleteButton"));
    actions->addWidget(add);
    actions->addWidget(rename);
    actions->addWidget(remove);
    actions->addStretch();
    auto* close = new QPushButton(t("common.done"), &dialog);
    close->setObjectName(QStringLiteral("primaryButton"));
    close->setProperty("automationId", QStringLiteral("projectDoneButton"));
    actions->addWidget(close);
    root->addLayout(actions);

    std::function<void()> refreshProjects;
    std::function<void()> refreshApplications;
    refreshProjects = [&]() {
        const QString previous = projects->currentItem() == nullptr ? model_.activeProjectId()
                                                                     : projects->currentItem()->data(Qt::UserRole).toString();
        projects->clear();
        int selected = 0;
        for (int index = 0; index < model_.projects().size(); ++index) {
            const Project& project = model_.projects().at(index);
            auto* item = new QListWidgetItem(projectDisplayName(project) +
                                                 (project.includeAllApplications ? t("project.allSuffix") : QString()), projects);
            item->setData(Qt::UserRole, project.id);
            if (project.id == previous) selected = index;
        }
        projects->setCurrentRow(selected);
    };
    refreshApplications = [&]() {
        applications->blockSignals(true);
        applications->clear();
        const QString projectId = projects->currentItem() == nullptr ? QString() : projects->currentItem()->data(Qt::UserRole).toString();
        const Project* project = model_.projectById(projectId);
        for (const ApplicationProfile& application : model_.applications()) {
            auto* item = new QListWidgetItem(application.name, applications);
            item->setData(Qt::UserRole, application.id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            const bool checked = project != nullptr && (project->includeAllApplications || project->applicationIds.contains(application.id));
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            if (project != nullptr && project->id == QStringLiteral("default")) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        applications->blockSignals(false);
        const bool editable = project != nullptr && project->id != QStringLiteral("default");
        remove->setToolTip(editable ? QString() : t("project.defaultRetained"));
        rename->setToolTip(remove->toolTip());
        rename->setEnabled(editable);
        remove->setEnabled(editable);
    };
    connect(projects, &QListWidget::currentRowChanged, &dialog, [&](int) { refreshApplications(); });
    connect(applications, &QListWidget::itemChanged, &dialog, [&](QListWidgetItem*) {
        if (projects->currentItem() == nullptr) return;
        QStringList ids;
        for (int row = 0; row < applications->count(); ++row) {
            QListWidgetItem* item = applications->item(row);
            if (item->checkState() == Qt::Checked) ids.push_back(item->data(Qt::UserRole).toString());
        }
        const auto id = projects->currentItem()->data(Qt::UserRole).toString();
        const auto* project = model_.projectById(id);
        const bool wasAll = project && project->includeAllApplications;
        if (model_.setProjectApplications(id, ids) && wasAll) refreshProjects();
    });
    connect(add, &QPushButton::clicked, &dialog, [&]() {
        bool ok = false;
        const QString name = QInputDialog::getText(&dialog, t("project.newTitle"), t("project.name"), QLineEdit::Normal, QString(), &ok);
        QString id;
        if (ok && model_.addProject(name, &id)) {
            model_.setActiveProject(id);
            refreshProjects();
            for (int row = 0; row < projects->count(); ++row) {
                if (projects->item(row)->data(Qt::UserRole).toString() == id) {
                    projects->setCurrentRow(row);
                    break;
                }
            }
        }
    });
    connect(rename, &QPushButton::clicked, &dialog, [&]() {
        if (projects->currentItem() == nullptr) return;
        const QString id = projects->currentItem()->data(Qt::UserRole).toString();
        bool ok = false;
        const QString name = QInputDialog::getText(&dialog, t("project.renameTitle"), t("project.name"), QLineEdit::Normal, model_.projectById(id)->name, &ok);
        if (ok && model_.renameProject(id, name)) refreshProjects();
    });
    connect(remove, &QPushButton::clicked, &dialog, [&]() {
        if (projects->currentItem() == nullptr) return;
        const auto id = projects->currentItem()->data(Qt::UserRole).toString();
        if (QMessageBox::question(&dialog, t("common.delete"), t("project.deleteQuestion").arg(projects->currentItem()->text())) != QMessageBox::Yes) return;
        if (!model_.removeProject(id)) {
            QMessageBox::warning(&dialog, t("common.delete"), t("project.defaultRetained"));
            return;
        }
        refreshProjects();
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    refreshProjects();
    dialog.exec();
    saveWorkspace();
    refreshProjectSelector();
    rebuildApplicationsList();
    rebuildTrayMenu();
}

void MainWindow::showAddApplicationDialog() {
    selectPage(2);
    updateStatusSummary(t("software.chooseToCreate"));
}

void MainWindow::showApplicationEditor(const QString& existingId, const int initialTab, const QString& softwareId) {
    ManagerModel staged = model_;
    QString applicationId = existingId;
    const bool creating = existingId.isEmpty();
    if (creating) {
        const auto* installation = staged.softwareById(softwareId);
        if (installation == nullptr || !staged.addApplication(softwareId, installation->displayName, &applicationId)) return;
        staged.mutableApplicationById(applicationId)->integrationAdapterId = preferredInstalledAdapter(*installation);
    }
    const ApplicationProfile* source = staged.applicationById(applicationId);
    if (source == nullptr) return;
    const ApplicationProfile original = *source;
    const SoftwareInstallation* software = model_.softwareById(original.softwareInstallationId);
    ApplicationEditorDialog dialog(this);
    dialog.setWindowTitle(creating ? t("apps.create") : t("editor.title").arg(original.name));
    dialog.resize(900, 680);
    auto* root = new QVBoxLayout(&dialog);
    auto* name = new QLineEdit(original.name, &dialog);
    name->setObjectName(QStringLiteral("applicationNameEdit"));
    auto* nameForm = new QFormLayout();
    nameForm->addRow(t("editor.name"), name);
    root->addLayout(nameForm);
    auto* tabs = new QTabWidget(&dialog);
    root->addWidget(tabs, 1);

    auto* basic = new QWidget(tabs);
    auto* basicLayout = new QFormLayout(basic);
    basicLayout->setContentsMargins(22, 22, 22, 22);
    basicLayout->setHorizontalSpacing(16);
    basicLayout->setVerticalSpacing(14);
    auto* installation = new QLabel(software == nullptr ? t("common.softwareUnavailable")
        : QStringLiteral("%1 %2 · %3").arg(software->displayName, software->version, software->architecture), basic);
    basicLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    basicLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    basicLayout->addRow(t("editor.softwareName"), installation);
    basicLayout->addRow(t("editor.installationPath"), mutedText(software == nullptr ? QString() : software->executablePath, basic));
    QLineEdit* configuration = nullptr;
    basicLayout->addRow(t("editor.integrationMethod"), new QLabel(integrationDisplay(original.integrationAdapterId), basic));
    const auto created = QDateTime::fromString(original.createdAtUtc, Qt::ISODateWithMs);
    basicLayout->addRow(t("editor.created"), new QLabel(created.isValid()
        ? created.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : t("editor.createdUnknown"), basic));
    QStringList projects;
    for (const auto& project : staged.projects())
        if (project.includeAllApplications || project.applicationIds.contains(applicationId))
            projects << projectDisplayName(project);
    basicLayout->addRow(t("editor.projects"), mutedText(projects.join(QStringLiteral(" · ")), basic));
    tabs->addTab(basic, t("editor.basic"));

    auto* channels = new QWidget(tabs);
    auto* channelsLayout = new QVBoxLayout(channels);
    channelsLayout->setContentsMargins(20, 18, 20, 18);
    auto* full = new QRadioButton(t("editor.fullDetail"), channels);
    full->setObjectName(QStringLiteral("assignmentFullAccess"));
    auto* selected = new QRadioButton(t("editor.selectedDetail"), channels);
    selected->setObjectName(QStringLiteral("assignmentSelected"));
    full->setChecked(original.assignmentMode == AssignmentMode::FullAccess);
    selected->setChecked(original.assignmentMode == AssignmentMode::Selected);
    channelsLayout->addWidget(full);
    channelsLayout->addWidget(selected);
    const int channelLimit = software == nullptr ? 16 : software->maximumCanChannels();
    auto* dynamicNote = mutedText(t("editor.dynamicNote").arg(channelLimit), channels);
    channelsLayout->addWidget(dynamicNote);
    const QVector<ChannelMapping> fullAccessMappings = model_.fullAccessMappings().mid(0, channelLimit);
    const auto frontend = software == nullptr ? FrontendPackage{} : frontendPackage(software->family);
    if (!frontend.id.isEmpty() && !frontend.fullAccess) {
        selected->setChecked(true);
        full->setEnabled(false);
        full->setToolTip(t("editor.selectedOnly"));
    }
    channelsLayout->addWidget(mutedText(t("editor.channelLimit").arg(channelLimit), channels));
    auto* table = new QTableWidget(channels);
    table->setObjectName(QStringLiteral("applicationChannelTable"));
    table->setColumnCount(4);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setHorizontalHeaderLabels({t("editor.logicalChannel"), t("editor.endpoint"), t("editor.state"), QString()});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    auto* mappingError = new QLabel(channels);
    mappingError->setObjectName(QStringLiteral("mappingValidationMessage"));
    mappingError->setWordWrap(true);
    mappingError->setStyleSheet(QStringLiteral("color: #b42318;"));
    channelsLayout->addWidget(mappingError);
    channelsLayout->addWidget(table, 1);
    tabs->addTab(channels, t("editor.channels"));

    // Preserve legacy rows even if this frontend now has a smaller limit. The
    // user must explicitly remove them; opening/saving must not truncate maps.
    QVector<QString> selectedIds(std::max(1, original.channelCount));
    for (const ChannelMapping& mapping : original.mappings)
        if (mapping.logicalChannel > 0 && mapping.logicalChannel <= selectedIds.size())
            selectedIds[mapping.logicalChannel - 1] = mapping.endpointId;
    QPushButton* saveButton = nullptr;
    auto updateSave = [&] {
        QVector<ChannelMapping> mappings;
        for (int i = 0; i < selectedIds.size(); ++i) mappings.push_back({i + 1, selectedIds.at(i)});
        const auto conflict = duplicateEndpointChannels(mappings, selectedIds.size());
        const bool duplicate = selected->isChecked() && !conflict.isEmpty();
        mappingError->setText(duplicate ? duplicateEndpointMessage(language_, conflict) : QString());
        mappingError->setVisible(duplicate);
        if (saveButton != nullptr) saveButton->setEnabled(!name->text().trimmed().isEmpty() && !duplicate &&
            (!selected->isChecked() || (!selectedIds.isEmpty() && selectedIds.size() <= channelLimit)));
    };
    auto updateEndpointChoices = [&] {
        if (!selected->isChecked()) return;
        for (int row = 0; row < selectedIds.size(); ++row) {
            auto* choice = qobject_cast<QComboBox*>(table->cellWidget(row, 1));
            if (choice == nullptr) continue;
            auto* items = qobject_cast<QStandardItemModel*>(choice->model());
            if (items == nullptr) continue;
            for (int index = 1; index < choice->count(); ++index) {
                const auto id = choice->itemData(index).toString();
                const auto* endpoint = model_.endpointById(id);
                const bool compatible = endpoint != nullptr && endpoint->state != EndpointState::Incompatible && endpoint->state != EndpointState::Disabled;
                int owner = 0;
                for (int other = 0; other < selectedIds.size(); ++other)
                    if (other != row && selectedIds.at(other) == id) { owner = other + 1; break; }
                // Keep a legacy duplicate visible so it can be corrected; new
                // choices cannot take an endpoint already assigned to a row.
                items->item(index)->setEnabled(compatible && (!owner || selectedIds.at(row) == id));
                // The application stylesheet sets a common text color; give
                // disabled choices an explicit muted brush in the item view.
                items->item(index)->setData(items->item(index)->isEnabled() ? QVariant() :
                    QVariant(QBrush(QColor(QStringLiteral("#8a94a3")))), Qt::ForegroundRole);
                // Keep the original label separate so repeated updates can
                // append/remove the status without accumulating suffixes.
                items->item(index)->setText(choice->itemData(index, Qt::UserRole + 1).toString() +
                    (owner ? t("mapping.endpointAssignedSuffix") : QString()));
                items->item(index)->setToolTip(QString());
            }
        }
    };
    std::function<void()> rebuildTable;
    rebuildTable = [&]() {
        const bool fixed = selected->isChecked();
        const int count = fixed ? selectedIds.size() : fullAccessMappings.size();
        for (int row = 0; row < table->rowCount(); ++row)
            for (int column = 0; column < table->columnCount(); ++column)
                if (auto* editor = table->cellWidget(row, column)) editor->hide();
        table->clearSpans();
        table->setRowCount(0);
        table->setRowCount(count + (fixed ? 1 : 0));
        for (int row = 0; row < count; ++row) {
            table->setRowHeight(row, 48);
            table->setItem(row, 0, new QTableWidgetItem(QStringLiteral("CAN %1").arg(row + 1)));
            auto* choice = new QComboBox(table);
            choice->setObjectName(QStringLiteral("channelEndpoint%1").arg(row + 1));
            choice->addItem(t("editor.unassigned"), QString());
            const QString endpointId = fixed ? selectedIds.at(row) : fullAccessMappings.at(row).endpointId;
            int current = 0;
            for (const Endpoint& endpoint : model_.endpoints()) {
                if (!isCanEndpoint(endpoint) || ((endpoint.historicalBinding || !model_.backendEnabledForEndpoint(endpoint)) && endpoint.id != endpointId)) continue;
                choice->addItem(endpointDisplay(endpoint, model_.backendDisplayName(endpoint)) +
                    (endpoint.historicalBinding ? QStringLiteral(" · ") + t("hardware.savedBinding") : QString()), endpoint.id);
                choice->setItemData(choice->count() - 1, choice->itemText(choice->count() - 1), Qt::UserRole + 1);
                if (endpoint.state == EndpointState::Incompatible || endpoint.state == EndpointState::Disabled) {
                    if (auto* choices = qobject_cast<QStandardItemModel*>(choice->model()))
                        choices->item(choice->count() - 1)->setEnabled(false);
                }
                if (endpoint.id == endpointId) current = choice->count() - 1;
            }
            choice->setCurrentIndex(current);
            choice->setEnabled(fixed);
            table->setCellWidget(row, 1, choice);
            const Endpoint* endpoint = model_.endpointById(endpointId);
            table->setCellWidget(row, 2, badgeLabel(endpoint == nullptr ? t("editor.unassigned") : endpointStateDisplay(endpoint->state),
                endpoint == nullptr ? QStringLiteral("muted") : stateKind(endpoint->state), table));
            connect(choice, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&, row, choice](int index) {
                if (!selected->isChecked() || row >= selectedIds.size()) return;
                selectedIds[row] = choice->itemData(index).toString();
                const auto* endpoint = model_.endpointById(selectedIds.at(row));
                table->setCellWidget(row, 2, badgeLabel(endpoint == nullptr ? t("editor.unassigned") : endpointStateDisplay(endpoint->state),
                    endpoint == nullptr ? QStringLiteral("muted") : stateKind(endpoint->state), table));
                updateEndpointChoices();
                updateSave();
            });
            if (fixed) {
                auto* remove = new QPushButton(t("common.delete"), table);
                remove->setAutoDefault(false);
                remove->setObjectName(QStringLiteral("ghostButton"));
                remove->setProperty("removeChannel", row + 1);
                remove->setAccessibleName(t("common.delete") + QStringLiteral(" CAN %1").arg(row + 1));
                table->setCellWidget(row, 3, remove);
                connect(remove, &QPushButton::clicked, &dialog, [&, row] {
                    if (QMessageBox::question(&dialog, t("common.delete"),
                            t("editor.removeChannelQuestion").arg(row + 1)) != QMessageBox::Yes) return;
                    selectedIds.removeAt(row);
                    rebuildTable();
                });
            }
        }
        if (fixed) {
            table->setSpan(count, 0, 1, 4);
            table->setRowHeight(count, 48);
            auto* add = new QPushButton(count < channelLimit ? t("editor.addChannel") : t("editor.channelLimit").arg(channelLimit), table);
            add->setAutoDefault(false);
            add->setObjectName(QStringLiteral("addChannelButton"));
            add->setProperty("automationId", QStringLiteral("addApplicationChannel"));
            add->setEnabled(count < channelLimit);
            table->setCellWidget(count, 0, add);
            connect(add, &QPushButton::clicked, &dialog, [&] {
                if (selectedIds.size() >= channelLimit) return;
                selectedIds.push_back(QString());
                rebuildTable();
                auto* choice = qobject_cast<QComboBox*>(table->cellWidget(selectedIds.size() - 1, 1));
                if (choice != nullptr) { table->scrollToItem(table->item(selectedIds.size() - 1, 0)); choice->setFocus(); choice->showPopup(); }
            });
        }
        dynamicNote->setVisible(!fixed);
        updateEndpointChoices();
        updateSave();
    };
    connect(selected, &QRadioButton::toggled, &dialog, [&](bool) { rebuildTable(); });
    rebuildTable();

    auto* integration = new QWidget(tabs);
    auto* integrationLayout = new QVBoxLayout(integration);
    integrationLayout->setContentsMargins(20, 18, 20, 18);
    auto* form = new QFormLayout();
    const auto configurationMetadata = software == nullptr ? QJsonObject{} : frontendPackage(software->family).configurationFile;
    if (configurationMetadata.value(QStringLiteral("enabled")).toBool()) {
        auto* row = new QWidget(integration);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        configuration = new QLineEdit(original.configurationPath, row);
        configuration->setObjectName(QStringLiteral("applicationConfigurationPath"));
        configuration->setPlaceholderText(t("editor.configurationHelp"));
        auto* browse = new QPushButton(t("common.browse"), row);
        layout->addWidget(configuration, 1);
        layout->addWidget(browse);
        form->addRow(t("editor.configuration"), row);
        connect(browse, &QPushButton::clicked, &dialog, [this, &dialog, configuration, configurationMetadata]() {
            const QString selected = QFileDialog::getOpenFileName(&dialog,
                t("editor.configuration"), configuration->text(), localizedMetadata(configurationMetadata.value(QStringLiteral("filter")), language_));
            if (!selected.isEmpty()) configuration->setText(selected);
        });
    }
    auto* arguments = new QLineEdit(original.commandLineArguments, integration);
    arguments->setObjectName(QStringLiteral("applicationArguments"));
    arguments->setPlaceholderText(t("editor.argumentsHelp"));
    form->addRow(t("editor.arguments"), arguments);
    integrationLayout->addLayout(form);
    integrationLayout->addWidget(mutedText(t("editor.connectionInApp"), integration));
    integrationLayout->addStretch();
    tabs->addTab(integration, t("editor.integration"));
    tabs->setCurrentIndex(std::max(0, std::min(initialTab, tabs->count() - 1)));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(t("editor.save"));
    saveButton = buttons->button(QDialogButtonBox::Save);
    updateSave();
    name->setPlaceholderText(t("apps.applicationName"));
    connect(name, &QLineEdit::textChanged, &dialog, [&](const QString&) { updateSave(); });
    buttons->button(QDialogButtonBox::Cancel)->setText(t("common.cancel"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        ApplicationProfile* changed = staged.mutableApplicationById(applicationId);
        if (changed == nullptr || name->text().trimmed().isEmpty()) return;
        changed->name = name->text().trimmed();
        changed->commandLineArguments = arguments->text();
        if (configuration != nullptr) changed->configurationPath = configuration->text().trimmed();
        if (selected->isChecked()) {
            if (!staged.replaceApplicationChannels(applicationId, selectedIds)) return;
        }
        staged.setApplicationAssignmentMode(
            applicationId, selected->isChecked() ? AssignmentMode::Selected
                                                  : AssignmentMode::FullAccess);
        dialog.accept();
    });
    // Enter confirms an open channel picker; it must not accidentally save a
    // partially configured application when that picker has already closed.
    for (auto* button : dialog.findChildren<QPushButton*>()) button->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Save)->setDefault(false);
    buttons->button(QDialogButtonBox::Save)->setShortcut(QKeySequence::Save);
    QTimer::singleShot(0, &dialog, [name]() { name->setFocus(); name->selectAll(); });
    if (dialog.exec() == QDialog::Accepted) {
        staged.refreshRuntimeStateFrom(model_);
        model_ = staged;
        saveWorkspace();
        rebuildHardwarePage();
        rebuildSoftwarePage();
        rebuildApplicationsList();
        rebuildTrayMenu();
    }
}

void MainWindow::showHardwareAssignmentDialog(const QString& endpointId) {
    const auto* endpoint = model_.endpointById(endpointId);
    if (endpoint == nullptr) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("hardwareAssignmentDialog"));
    dialog.setWindowTitle(t("hardware.assignTitle"));
    dialog.resize(640, 420);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(sectionTitle(endpointDisplay(*endpoint, model_.backendDisplayName(*endpoint)), &dialog));
    layout->addWidget(mutedText(t("hardware.bindingHelp"), &dialog));
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("hardwareApplicationList"));
    for (const auto& application : model_.applications()) {
        QStringList channels;
        for (const auto& mapping : model_.effectiveMappingsForApplication(application.id))
            if (mapping.endpointId == endpointId) channels << QStringLiteral("CAN %1").arg(mapping.logicalChannel);
        if (channels.isEmpty()) continue;
        auto* item = new QListWidgetItem(application.name + QStringLiteral(" · ") + channels.join(QStringLiteral(", ")), list);
        item->setData(Qt::UserRole, application.id);
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    }
    layout->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* edit = buttons->addButton(t("common.settings"), QDialogButtonBox::ActionRole);
    edit->setEnabled(list->count() > 0);
    if (list->count()) list->setCurrentRow(0);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(edit, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted && list->currentItem())
        showApplicationEditor(list->currentItem()->data(Qt::UserRole).toString(), 1);
}

void MainWindow::runFrontendOperation(const QString& applicationId, const QString& operation,
                                        const QJsonObject& extraContext) {
    const auto* application = model_.applicationById(applicationId);
    if (application == nullptr) return;
    const auto* software = model_.softwareById(application->softwareInstallationId);
    if (software == nullptr) return;
    LaunchEnvironment environment;
    environment.applicationDirectory = QCoreApplication::applicationDirPath();
    environment.language = language_;
    const auto plan = LaunchCoordinator::prepare(model_, applicationId, brokerInventoryFresh_, false, environment);
    QJsonObject context{{"plan", plan.toJson()}, {"workspacePath", workspacePath_},
        {"applicationDirectory", environment.applicationDirectory}, {"applicationId", applicationId},
        {"executablePath", software->executablePath}, {"language", static_cast<int>(language_)},
        {"evidenceDirectory", operationEvidenceDirectory(workspacePath_, applicationId, operation)}};
    for (auto it = extraContext.begin(); it != extraContext.end(); ++it) context.insert(it.key(), it.value());
    const auto family = software->family;
    auto* watcher = new QFutureWatcher<FrontendResult>(this);
    connect(watcher, &QFutureWatcher<FrontendResult>::finished, this,
        [this, watcher, applicationId, extraContext]() {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (!result.ok()) { QMessageBox::warning(this, t("nav.software"), result.error); return; }
            const auto confirmation = result.value.value(QStringLiteral("confirmation")).toObject();
            const auto question = confirmation.value(QStringLiteral("message")).toString();
            if (!question.isEmpty()) {
                if (QMessageBox::question(this, t("nav.software"), question,
                        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
                auto nextContext = extraContext;
                const auto supplied = confirmation.value(QStringLiteral("context")).toObject();
                for (auto it = supplied.begin(); it != supplied.end(); ++it) nextContext.insert(it.key(), it.value());
                runFrontendOperation(applicationId, confirmation.value(QStringLiteral("operation")).toString(), nextContext);
                return;
            }
            const auto message = result.value.value(QStringLiteral("message")).toString();
            if (!message.isEmpty()) QMessageBox::information(this, t("nav.software"), message);
        });
    watcher->setFuture(QtConcurrent::run([family, operation, context]() {
        return invokeFrontend(family, operation, context);
    }));
}

void MainWindow::showDeveloperDeploymentDialog(const QString& applicationId) {
    const auto* application = model_.applicationById(applicationId);
    if (application == nullptr) return;
    const auto* software = model_.softwareById(application->softwareInstallationId);
    if (isProduction()) {
        runFrontendOperation(applicationId, QStringLiteral("deploy"));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("开发者部署兼容层 · 仅预览"));
    dialog.resize(720, 560);
    auto* root = new QVBoxLayout(&dialog);
    root->addWidget(sectionTitle(QStringLiteral("为自有软件部署 UCAL 兼容层"), &dialog));
    root->addWidget(mutedText(QStringLiteral("推荐导出集成包，由软件开发者放入自己的安装器。应用目录部署必须具备备份、manifest 和一键恢复。"), &dialog));
    auto* ownership = new QCheckBox(QStringLiteral("我确认拥有或已获授权修改和分发此软件"), &dialog);
    root->addWidget(ownership);
    auto* target = cardFrame(&dialog);
    auto* targetLayout = new QVBoxLayout(target);
    targetLayout->addWidget(sectionTitle(application->name, target));
    targetLayout->addWidget(mutedText(software == nullptr ? QStringLiteral("软件安装不可用") : software->executablePath, target));
    root->addWidget(target);
    auto* plan = new QTextBrowser(&dialog);
    plan->setHtml(QStringLiteral("<b>预览计划（不会执行）</b><ol>"
                                 "<li>拒绝 Windows 系统目录；仅对用户明确授权的软件执行</li>"
                                 "<li>检查目标位数、依赖名和必要导出</li>"
                                 "<li>备份已有 DLL，并记录 SHA-256 审计信息</li>"
                                 "<li>写入 UCAL-owned 文件和恢复 manifest</li>"
                                 "<li>验证加载后保留一键恢复入口</li></ol>"
                                 "<p><b>POC 不会复制、覆盖或删除任何文件。</b></p>"));
    root->addWidget(plan, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* exportPlan = buttons->addButton(QStringLiteral("导出集成计划（POC）"), QDialogButtonBox::ActionRole);
    exportPlan->setEnabled(false);
    root->addWidget(buttons);
    connect(ownership, &QCheckBox::toggled, exportPlan, &QPushButton::setEnabled);
    connect(exportPlan, &QPushButton::clicked, &dialog, [&]() {
        QMessageBox::information(&dialog, QStringLiteral("POC"), QStringLiteral("正式版本会导出清单与开发者集成包；当前不会写文件。"));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::createProfileShortcutAction(const QString& applicationId) {
    ApplicationProfile* application = model_.mutableApplicationById(applicationId);
    if (application == nullptr) return;
    if (isProduction()) {
        const QString shortcutPath = defaultProfileShortcutPath(application->name);
        const auto* software = model_.softwareById(application->softwareInstallationId);
        QString error;
        if (!createProfileShortcut(QCoreApplication::applicationFilePath(), applicationId,
                                   application->name, shortcutPath, &error,
                                   QFileInfo(workspacePath_).absoluteFilePath() ==
                                       QFileInfo(ManagerModel::productionStoragePath()).absoluteFilePath()
                                       ? QString() : workspacePath_, software == nullptr ? QString() : software->executablePath)) {
            QMessageBox::warning(this, t("apps.menu.shortcut"), error);
            return;
        }
        application->shortcutCreated = true;
        saveWorkspace();
        updateStatusSummary(QStringLiteral("%1 · %2 · %3")
                                .arg(application->name, t("apps.shortcutCreated"),
                                     QDir::toNativeSeparators(shortcutPath)));
        return;
    }
    application->shortcutCreated = true;
    if (isRealistic()) {
        updateStatusSummary(QStringLiteral("%1 · %2").arg(application->name, t("apps.shortcutCreated")));
    } else {
        QMessageBox::information(this, QStringLiteral("桌面快捷方式 · POC"),
                                 QStringLiteral("正式快捷方式将使用 UCAL Manager 图标，目标为：\n\n"
                                                "UcalManager.exe --profile %1\n\n"
                                                "当前 POC 只展示计划，不创建真实快捷方式。").arg(applicationId));
    }
    saveWorkspace();
}

void MainWindow::launchProfile(const QString& applicationId) {
    emit profileLaunchRequested(applicationId);
    if (isProduction() && runtimeActionsEnabled_ &&
        (!brokerInventoryFresh_ || (discoveryWatcher_ != nullptr && discoveryWatcher_->isRunning()))) {
        if (!pendingLaunchProfileIds_.contains(applicationId)) {
            pendingLaunchProfileIds_.push_back(applicationId);
        }
        updateStatusSummary(t("apps.refreshBeforeLaunch"));
        if (discoveryWatcher_ == nullptr || !discoveryWatcher_->isRunning()) {
            refreshRuntimeInventory(false);
        }
        return;
    }
    executeProfileLaunch(applicationId);
}

void MainWindow::launchAllProfiles() {
    const QVector<ApplicationProfile> applications =
        model_.applicationsForProject(model_.activeProjectId());
    if (applications.isEmpty()) {
        updateStatusSummary(t("apps.startAllEmpty"));
        return;
    }
    updateStatusSummary(t("apps.startAllQueued").arg(applications.size()));
    for (const ApplicationProfile& application : applications) {
        launchProfile(application.id);
    }
}

void MainWindow::handleExternalRequest(const QString& applicationId) {
    if (applicationId.isEmpty()) showAndRaise();
    else launchProfile(applicationId);
}

void MainWindow::runPendingProfileLaunches() {
    if (pendingLaunchProfileIds_.isEmpty()) return;
    const QStringList applicationIds = pendingLaunchProfileIds_;
    pendingLaunchProfileIds_.clear();
    const auto generation = backendCatalogGeneration_;
    const auto topology = hardwareChangeMonitor_ == nullptr ? 0 : hardwareChangeMonitor_->generation();
    QTimer::singleShot(0, this, [this, applicationIds, generation, topology]() {
        for (const QString& applicationId : applicationIds) {
            if (generation != backendCatalogGeneration_ ||
                (hardwareChangeMonitor_ != nullptr && topology != hardwareChangeMonitor_->generation())) {
                launchProfile(applicationId);
            } else {
                executeProfileLaunch(applicationId);
            }
        }
    });
}

void MainWindow::showActiveLaunch(const QString& applicationId) {
    const auto launch = activeLaunches_.constFind(applicationId);
    if (launch == activeLaunches_.cend() || launch->process.isNull() ||
        launch->process->state() == QProcess::NotRunning) {
        QMessageBox::information(this, t("apps.menu.last"), t("apps.notRunning"));
        return;
    }
    activateOwnedApplicationWindow(launch->process->processId());
}

void MainWindow::stopActiveLaunch(const QString& applicationId) {
    if (!isProduction()) {
        QMessageBox::information(this, t("apps.menu.stop"), t("apps.reviewNoProcesses"));
        return;
    }
    const auto launch = activeLaunches_.find(applicationId);
    if (launch == activeLaunches_.end() || launch->process.isNull() ||
        launch->process->state() == QProcess::NotRunning) {
        QMessageBox::warning(this, t("apps.menu.stop"), t("apps.notRunning"));
        return;
    }
    QProcess* process = launch->process;
    launch->stopping = true;
    if (launch->resourceSlot > 0) {
        process->write("stop\n");
        process->waitForBytesWritten(100);
    } else process->terminate();
    QTimer::singleShot(7000, process, [process]() {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    updateStatusSummary(t("apps.stopping").arg(launch->applicationName));
}

void MainWindow::startProfileProcess(const QString& applicationId, const QString& applicationName,
                                     const ProductLaunchSpec& spec,
                                     const QSharedPointer<QTemporaryDir>& temporaryDirectory) {
    auto* process = new QProcess(this);
    process->setProgram(spec.program);
    process->setArguments(spec.arguments);
    process->setWorkingDirectory(spec.workingDirectory);
    process->setProcessEnvironment(spec.environment);
    ActiveLaunch launch;
    launch.process = process;
    launch.temporaryDirectory = temporaryDirectory;
    launch.resourceSlot = spec.resourceSlot;
    launch.applicationName = applicationName;
    launch.evidenceFiles = spec.evidenceFiles;
    activeLaunches_.insert(applicationId, launch);
    connect(process, &QProcess::started, this,
            [this, applicationName, process]() {
        updateStatusSummary(t("apps.started")
                                .arg(applicationName)
                                .arg(process->processId()));
        rebuildApplicationsList();
        rebuildTrayMenu();
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, applicationId, applicationName, process]
            (const QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        const QString failure = t("apps.startFailed")
            .arg(applicationName, process->errorString());
        lastLaunchErrors_[applicationId] = failure;
        const QString logs = QDir(QFileInfo(workspacePath_).absolutePath())
            .filePath(QStringLiteral("run-logs"));
        if (QDir().mkpath(logs)) saveOperationEvidence(
            QDir(logs).filePath(applicationId + QStringLiteral(".last.log")), failure.toUtf8());
        const auto active = activeLaunches_.constFind(applicationId);
        if (active != activeLaunches_.cend() && active->process == process) {
            activeLaunches_.remove(applicationId);
        }
        rebuildApplicationsList();
        rebuildTrayMenu();
        process->deleteLater();
        QMessageBox::critical(this, t("apps.launchTitle"), failure);
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, applicationId, applicationName, process]
            (const int exitCode, const QProcess::ExitStatus exitStatus) {
        const auto active = activeLaunches_.constFind(applicationId);
        // Bridge stops are cooperative, so their exit status still reports the shutdown result.
        const bool expectedTermination = active != activeLaunches_.cend() &&
            active->stopping && active->resourceSlot <= 0;
        QByteArray evidence = process->readAllStandardError() + process->readAllStandardOutput();
        if (active != activeLaunches_.cend()) {
            for (const auto& path : active->evidenceFiles) {
                QFile file(path);
                if (file.open(QIODevice::ReadOnly)) {
                    file.seek(qMax<qint64>(0, file.size() - 512 * 1024));
                    evidence += file.read(512 * 1024);
                }
            }
        }
        const bool crashed = exitStatus == QProcess::CrashExit;
        const QString result = t("apps.stopped").arg(applicationName).arg(exitCode)
            .arg(crashed ? t("apps.crashed") : QString());
        if (!expectedTermination && (exitCode != 0 || crashed)) {
            lastLaunchErrors_[applicationId] = result
                + QStringLiteral("\n") + QString::fromUtf8(evidence.left(8192));
        }
        const QString logs = QDir(QFileInfo(workspacePath_).absolutePath()).filePath(QStringLiteral("run-logs"));
        if (QDir().mkpath(logs)) saveOperationEvidence(QDir(logs).filePath(applicationId + QStringLiteral(".last.log")), evidence);
        if (active != activeLaunches_.cend() && active->process == process) {
            activeLaunches_.remove(applicationId);
        }
        updateStatusSummary(result);
        rebuildApplicationsList();
        rebuildTrayMenu();
        process->deleteLater();
    });
    lastLaunchErrors_.remove(applicationId);
    updateStatusSummary(t("apps.starting").arg(applicationName));
    process->start();
}

void MainWindow::executeProfileLaunch(const QString& applicationId, const bool nativeLaunch,
                                      const bool planOnly) {
    const ApplicationProfile* application = model_.applicationById(applicationId);
    if (application == nullptr) return;
    const SoftwareInstallation* software = model_.softwareById(application->softwareInstallationId);
    if (isProduction()) {
        if (!planOnly && isProfileActive(applicationId)) {
            showActiveLaunch(applicationId);
            return;
        }
        LaunchEnvironment environment;
        environment.applicationDirectory = QCoreApplication::applicationDirPath();
        environment.language = language_;
        LaunchPlan plan = LaunchCoordinator::prepare(
            model_, applicationId, brokerInventoryFresh_, nativeLaunch, environment);
        if (planOnly || !plan.ready) {
            QMessageBox check(QMessageBox::Information,
                planOnly ? t("apps.menu.check") : nativeLaunch ? t("apps.nativeTitle") : t("apps.launchTitle"),
                userLaunchSummary(plan, language_), QMessageBox::Ok, this);
            if (lastLaunchErrors_.contains(applicationId))
                check.setDetailedText(lastLaunchErrors_.value(applicationId));
            check.exec();
            updateStatusSummary(t("apps.launchCheck")
                                    .arg(application->name,
                                         t(plan.ready ? "apps.checkReady"
                                                      : "apps.checkBlocked")));
            return;
        }

        ProductLaunchSpec spec;
        QSharedPointer<QTemporaryDir> temporaryDirectory;
        if (nativeLaunch) {
            spec.program = plan.softwareExecutable;
            spec.arguments = applicationArguments(plan);
            spec.workingDirectory = QFileInfo(plan.softwareExecutable).absolutePath();
            spec.environment = QProcessEnvironment::systemEnvironment();
        } else {
            temporaryDirectory.reset(new QTemporaryDir());
            if (!temporaryDirectory->isValid()) {
                QMessageBox::critical(this, t("apps.launchTitle"),
                                      t("apps.tempDataFailed"));
                return;
            }
            QSet<int> reservedResourceSlots;
            for (auto launch = activeLaunches_.cbegin(); launch != activeLaunches_.cend();
                 ++launch) {
                if (launch->resourceSlot > 0 && !launch->process.isNull() &&
                    launch->process->state() != QProcess::NotRunning) {
                    reservedResourceSlots.insert(launch->resourceSlot);
                }
            }
            spec = ProductLauncher::prepare(plan, QCoreApplication::applicationDirPath(),
                                            temporaryDirectory->path(), language_,
                                            reservedResourceSlots);
            if (!spec.valid()) {
                QMessageBox::warning(this, t("apps.launchTitle"), spec.error);
                return;
            }
        }

        startProfileProcess(applicationId, application->name, spec, temporaryDirectory);
        return;
    }
    if (isRealistic()) {
        const QString assignment = application->assignmentMode == AssignmentMode::FullAccess
            ? t("editor.full") : t("apps.channelCount").arg(application->channelCount);
        QMessageBox::information(this, nativeLaunch ? t("apps.nativeTitle") : t("apps.launchTitle"),
                                 t("apps.launchDetails")
                                     .arg(application->name,
                                          software == nullptr ? t("common.offline") : software->executablePath,
                                          nativeLaunch ? t("apps.menu.native") : application->integrationName,
                                          assignment, t("common.configured")));
        return;
    }
    const QString details = nativeLaunch
        ? QStringLiteral("将直接启动原始 EXE，不应用 UCAL 接入方式和通道映射。")
        : QStringLiteral("接入：%1\n硬件：%2\n\n正式版本会生成不可变运行快照并执行预检。")
              .arg(application->integrationName, assignmentModeText(application->assignmentMode));
    QMessageBox::information(this, nativeLaunch ? QStringLiteral("原生启动 · POC") : QStringLiteral("启动计划 · POC"),
                             QStringLiteral("%1\n%2\n\n%3\n\n当前 POC 不启动外部程序。")
                                 .arg(application->name,
                                      software == nullptr ? QStringLiteral("软件安装不可用") : software->executablePath,
                                      details));
}

void MainWindow::refreshInventoryAction(const QString& target) {
    if (isProduction()) {
        Q_UNUSED(target)
        refreshRuntimeInventory(true);
        return;
    }
    if (isRealistic()) {
        Q_UNUSED(target)
        updateStatusSummary(t("status.scanComplete"));
        return;
    }
    updateStatusSummary(QStringLiteral("已刷新演示清单 · 映射保持不变"));
    QMessageBox::information(this, QStringLiteral("扫描 · POC"),
                             QStringLiteral("正式版本会异步扫描：%1。\n\n不会全盘搜索 DLL，也不会因为文件名匹配就宣称兼容。当前 POC 保持演示清单不变。")
                                 .arg(target));
}

void MainWindow::saveWorkspace(const bool notify) {
    if (isRealistic()) {
        if (notify) updateStatusSummary(t("common.configured"));
        return;
    }
    QString error;
    if (!model_.saveToFile(workspacePath_, &error)) {
        QMessageBox::warning(this, QStringLiteral("UCAL Manager"),
                             t("config.saveFailed").arg(error));
        return;
    }
    if (notify) updateStatusSummary(t("config.saved"));
}

void MainWindow::savePreferences() {
    if (!saveUserPreferences(preferences_)) {
        QMessageBox::warning(this, QStringLiteral("UCAL Manager"),
                             t("settings.saveFailed"));
    }
}

void MainWindow::createTray() {
    tray_ = new QSystemTrayIcon(managerIcon(), this);
    tray_->setToolTip(t(usesProductCopy() ? "window.realistic" : "window.explanatory"));
    rebuildTrayMenu();
    connect(tray_, &QSystemTrayIcon::activated, this, [this](const QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) showAndRaise();
    });
    tray_->show();
}

void MainWindow::rebuildTrayMenu() {
    if (tray_ == nullptr) return;
    if (tray_->contextMenu() != nullptr) tray_->contextMenu()->deleteLater();
    auto* menu = new QMenu(this);
    auto* open = menu->addAction(t("common.open"));
    const Project* project = model_.projectById(model_.activeProjectId());
    const QString projectName = project == nullptr ? t("project.default") : projectDisplayName(*project);
    auto* projectTitle = menu->addAction(t("tray.currentProject").arg(projectName));
    projectTitle->setEnabled(false);
    menu->addSeparator();
    const auto applications = model_.applicationsForProject(model_.activeProjectId());
    for (int index = 0; index < std::min(4, applications.size()); ++index) {
        const ApplicationProfile application = applications.at(index);
        auto* launch = menu->addAction(t("tray.launchApp").arg(application.name));
        connect(launch, &QAction::triggered, this, [this, application]() {
            showAndRaise();
            launchProfile(application.id);
        });
    }
    menu->addAction(t("apps.startAll"), this, [this]() {
        showAndRaise();
        launchAllProfiles();
    });
    menu->addSeparator();
    menu->addAction(t("tray.rescan"), this, [this]() { refreshInventoryAction(QStringLiteral("硬件通道")); });
    menu->addSeparator();
    auto* quit = menu->addAction(t("common.exit"));
    connect(open, &QAction::triggered, this, &MainWindow::showAndRaise);
    connect(quit, &QAction::triggered, this, [this]() { forceQuit_ = true; tray_->hide(); qApp->quit(); });
    tray_->setContextMenu(menu);
}

void MainWindow::showAndRaise() {
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::updateStatusSummary(const QString& message) {
    if (statusSummary_ == nullptr) return;
    if (!message.isEmpty()) statusBar()->showMessage(message, 15000);
    int online = 0;
    int canEndpoints = 0;
    for (const Endpoint& endpoint : model_.endpoints()) {
        if (!isCanEndpoint(endpoint) || endpoint.historicalBinding || !model_.backendEnabledForEndpoint(endpoint)) continue;
        ++canEndpoints;
        if (endpoint.state == EndpointState::Online) ++online;
    }
    if (connectionStatus_ != nullptr) {
        connectionStatus_->setText(t(brokerInventoryFresh_ ? "status.serviceOnline" : "status.servicePending"));
        connectionStatus_->setStyleSheet(brokerInventoryFresh_ ? QStringLiteral("color:#18794e; border-right:1px solid #e4e7ec;")
                                                              : QStringLiteral("color:#667085; border-right:1px solid #e4e7ec;"));
    }
    if (isProduction() && !brokerInventoryFresh_) {
        statusSummary_->setText(t("hardware.productionCached").arg(canEndpoints));
        return;
    }
    const QString summary = t("status.summary")
        .arg(model_.applications().size()).arg(canEndpoints).arg(online);
    statusSummary_->setText(usesProductCopy() ? summary : QStringLiteral("POC · %1").arg(summary));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!forceQuit_ && closeToTray_ && tray_ != nullptr && tray_->isVisible()) {
        hide();
        tray_->showMessage(QStringLiteral("UCAL Manager"), t("tray.running"),
                           QSystemTrayIcon::Information, 2000);
        event->ignore();
        return;
    }
    if (!forceQuit_ && askBeforeExit_) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, t("tray.exitTitle"), t("tray.exitQuestion"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) { event->ignore(); return; }
    }
    QMainWindow::closeEvent(event);
}

}  // namespace Ucal::Manager
