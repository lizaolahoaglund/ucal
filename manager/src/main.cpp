#include "MainWindow.h"
#include "Branding.h"
#include "control/PortablePaths.h"
#include "SingleInstance.h"
#include "control/UiPresentation.h"
#include "control/UserPreferences.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QTimer>

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);
    app.setWindowIcon(Ucal::Manager::managerIcon());
    QCoreApplication::setOrganizationName(QStringLiteral("UCAL"));
    QCoreApplication::setApplicationName(QStringLiteral("Ucal Manager"));

    QString migrationError;
    if (!Ucal::Manager::migrateLegacyState(&migrationError)) {
        QMessageBox::critical(nullptr, QStringLiteral("UCAL Manager"), migrationError);
        return 5;
    }
    const QStringList arguments = app.arguments();
    const Ucal::Manager::PresentationMode presentationMode =
        Ucal::Manager::presentationModeFromArguments(arguments, app.applicationFilePath());
    Ucal::Manager::UiLanguage language = Ucal::Manager::loadUiLanguagePreference();
    const int languageArgument = arguments.indexOf(QStringLiteral("--language"));
    if (languageArgument >= 0 && languageArgument + 1 < arguments.size()) {
        language = Ucal::Manager::uiLanguageFromKey(arguments.at(languageArgument + 1));
    }

    const int screenshotIndex = arguments.indexOf(QStringLiteral("--screenshot"));
    const bool screenshotMode = screenshotIndex >= 0 && screenshotIndex + 1 < arguments.size();
    const bool liveMode = arguments.contains(QStringLiteral("--live"));
    const int workspaceArgument = arguments.indexOf(QStringLiteral("--workspace"));
    const QString workspacePath = workspaceArgument >= 0 && workspaceArgument + 1 < arguments.size()
        ? arguments.at(workspaceArgument + 1) : QString();
    const int pageArgument = arguments.indexOf(QStringLiteral("--page"));
    const int dialogArgument = arguments.indexOf(QStringLiteral("--dialog"));
    int launchProfileArgument = arguments.indexOf(QStringLiteral("--profile"));
    if (launchProfileArgument < 0) {
        launchProfileArgument = arguments.indexOf(QStringLiteral("--launch-profile"));
    }
    const QString launchProfileId =
        launchProfileArgument >= 0 && launchProfileArgument + 1 < arguments.size()
            ? arguments.at(launchProfileArgument + 1).trimmed() : QString();
    const QString effectiveWorkspacePath = workspacePath.isEmpty()
        ? presentationMode == Ucal::Manager::PresentationMode::Production
            ? Ucal::Manager::ManagerModel::productionStoragePath()
            : Ucal::Manager::ManagerModel::defaultStoragePath()
        : QFileInfo(workspacePath).absoluteFilePath();

    Ucal::Manager::SingleInstanceController instanceController;
    if (!screenshotMode) {
        QString instanceError;
        if (!instanceController.acquirePrimary(effectiveWorkspacePath, &instanceError)) {
            if (!instanceError.isEmpty()) {
                QMessageBox::critical(nullptr, QStringLiteral("UCAL Manager"), instanceError);
                return 3;
            }
            QString forwardError;
            if (Ucal::Manager::SingleInstanceController::forwardToPrimary(
                    effectiveWorkspacePath, launchProfileId, &forwardError)) {
                return 0;
            }
            if (!instanceController.acquirePrimary(effectiveWorkspacePath, &instanceError)) {
                QMessageBox::critical(nullptr, QStringLiteral("UCAL Manager"),
                                      instanceError.isEmpty() ? forwardError : instanceError);
                return 4;
            }
        }
    }

    Ucal::Manager::MainWindow window(presentationMode, language, !screenshotMode,
                                  !screenshotMode || liveMode, effectiveWorkspacePath);
    QObject::connect(&instanceController,
                     &Ucal::Manager::SingleInstanceController::requestReceived,
                     &window, &Ucal::Manager::MainWindow::handleExternalRequest);
    if (pageArgument >= 0 && pageArgument + 1 < arguments.size()) {
        const QString page = arguments.at(pageArgument + 1).toLower();
        if (page == QStringLiteral("hardware") || page == QStringLiteral("devices")) window.selectPage(1);
        else if (page == QStringLiteral("software")) window.selectPage(2);
        else if (page == QStringLiteral("drivers") || page == QStringLiteral("backends")) window.selectPage(3);
        else if (page == QStringLiteral("settings")) window.selectPage(4);
    }
    const Ucal::Manager::UserPreferences preferences = Ucal::Manager::loadUserPreferences();
    const bool startMinimized = Ucal::Manager::shouldStartMinimized(
        preferences, presentationMode == Ucal::Manager::PresentationMode::Production,
        screenshotMode, QSystemTrayIcon::isSystemTrayAvailable(),
        arguments.contains(QStringLiteral("--start-minimized")));
    if (!startMinimized) window.show();

    if (!launchProfileId.isEmpty()) {
        const QString applicationId = launchProfileId;
        QTimer::singleShot(100, &window, [&window, applicationId]() {
            window.launchProfile(applicationId);
        });
    }

    if (dialogArgument >= 0 && dialogArgument + 1 < arguments.size()) {
        const QString dialog = arguments.at(dialogArgument + 1).toLower();
        if (dialog == QStringLiteral("application") || dialog == QStringLiteral("application-channels") ||
            dialog == QStringLiteral("application-integration")) {
            const int tabIndex = dialog == QStringLiteral("application-channels") ? 1
                : dialog == QStringLiteral("application-integration") ? 2 : 0;
            QTimer::singleShot(250, &window, [&window, tabIndex]() { window.showApplicationEditorForReview(tabIndex); });
        } else if (dialog == QStringLiteral("project") || dialog == QStringLiteral("add-application") ||
                   dialog == QStringLiteral("hardware-assignment")) {
            QTimer::singleShot(250, &window, [&window, dialog]() { window.showDialogForReview(dialog); });
        }
    }

    if (screenshotMode) {
        const QString outputPath = QFileInfo(arguments.at(screenshotIndex + 1)).absoluteFilePath();
        QDir().mkpath(QFileInfo(outputPath).absolutePath());
        QTimer::singleShot(liveMode ? 7000 : 900, &window, [&app, &window, outputPath]() {
            QWidget* target = QApplication::activeModalWidget();
            if (target == nullptr) target = &window;
            window.repaint();
            target->repaint();
            QApplication::processEvents();
            QPixmap snapshot(target->size());
            snapshot.fill(Qt::transparent);
            target->render(&snapshot);
            const bool saved = snapshot.save(outputPath);
            app.exit(saved ? 0 : 2);
        });
    }

    return app.exec();
}
