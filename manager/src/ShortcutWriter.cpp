#include "ShortcutWriter.h"
#include "Branding.h"
#include "control/PortablePaths.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <objbase.h>
#include <shlobj.h>

#include <vector>

namespace Ucal::Manager {
namespace {

QString hresultText(const HRESULT value) {
    return QStringLiteral("Windows shortcut operation failed (HRESULT 0x%1).")
        .arg(static_cast<qulonglong>(static_cast<unsigned long>(value)), 8, 16,
             QLatin1Char('0'));
}

}  // namespace

QString defaultProfileShortcutPath(const QString& applicationName) {
    QString safeName = applicationName.trimmed();
    safeName.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*])")),
                     QStringLiteral("_"));
    while (safeName.endsWith(QLatin1Char('.')) || safeName.endsWith(QLatin1Char(' '))) {
        safeName.chop(1);
    }
    if (safeName.isEmpty()) safeName = QStringLiteral("UCAL application");
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return QDir(desktop).filePath(QStringLiteral("%1 - UCAL.lnk").arg(safeName));
}

bool createProfileShortcut(const QString& managerExecutable, const QString& applicationId,
                           const QString& applicationName, const QString& shortcutPath,
                           QString* error, const QString& workspacePath,
                           const QString& softwareExecutable) {
    const QFileInfo executable(managerExecutable);
    if (!executable.isFile() || applicationId.trimmed().isEmpty() ||
        shortcutPath.trimmed().isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Shortcut input is incomplete.");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(shortcutPath).absolutePath())) {
        if (error != nullptr) *error = QStringLiteral("Shortcut directory is unavailable.");
        return false;
    }
    const auto dataDirectory = workspacePath.isEmpty() ? portableDataDirectory() : QFileInfo(workspacePath).absolutePath();
    QString iconError;
    const auto iconPath = createShortcutIcon(softwareExecutable, QDir(dataDirectory).filePath(QStringLiteral("icons")), &iconError);
    if (!iconError.isEmpty()) { if (error) *error = iconError; return false; }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        if (error != nullptr) *error = hresultText(initialized);
        return false;
    }

    IShellLinkW* link = nullptr;
    HRESULT status = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (SUCCEEDED(status)) {
        const std::wstring target = QDir::toNativeSeparators(
            executable.absoluteFilePath()).toStdWString();
        QString command = QStringLiteral("--profile \"%1\"").arg(applicationId);
        if (!workspacePath.isEmpty()) command += QStringLiteral(" --workspace \"%1\"")
            .arg(QDir::toNativeSeparators(QFileInfo(workspacePath).absoluteFilePath()));
        const std::wstring arguments = command.toStdWString();
        const std::wstring workingDirectory = QDir::toNativeSeparators(
            executable.absolutePath()).toStdWString();
        const std::wstring description = QStringLiteral("Launch %1 through UCAL Manager")
                                             .arg(applicationName).toStdWString();
        status = link->SetPath(target.c_str());
        if (SUCCEEDED(status)) status = link->SetArguments(arguments.c_str());
        if (SUCCEEDED(status)) status = link->SetWorkingDirectory(workingDirectory.c_str());
        if (SUCCEEDED(status)) status = link->SetDescription(description.c_str());
        const auto icon = iconPath.isEmpty() ? target : QDir::toNativeSeparators(iconPath).toStdWString();
        if (SUCCEEDED(status)) status = link->SetIconLocation(icon.c_str(), 0);
        if (SUCCEEDED(status)) {
            IPersistFile* persist = nullptr;
            status = link->QueryInterface(IID_IPersistFile,
                                          reinterpret_cast<void**>(&persist));
            if (SUCCEEDED(status)) {
                const std::wstring output = QDir::toNativeSeparators(
                    QFileInfo(shortcutPath).absoluteFilePath()).toStdWString();
                status = persist->Save(output.c_str(), TRUE);
                persist->Release();
            }
        }
        link->Release();
    }
    if (uninitialize) CoUninitialize();
    if (FAILED(status)) {
        if (error != nullptr) *error = hresultText(status);
        return false;
    }
    return true;
}

bool readProfileShortcut(const QString& shortcutPath, QString* target, QString* arguments,
                         QString* error, QString* iconPath) {
    if (target == nullptr || arguments == nullptr || !QFileInfo(shortcutPath).isFile()) {
        if (error != nullptr) *error = QStringLiteral("Shortcut inspection input is incomplete.");
        return false;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        if (error != nullptr) *error = hresultText(initialized);
        return false;
    }
    IShellLinkW* link = nullptr;
    HRESULT status = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (SUCCEEDED(status)) {
        IPersistFile* persist = nullptr;
        status = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist));
        if (SUCCEEDED(status)) {
            const std::wstring input = QDir::toNativeSeparators(
                QFileInfo(shortcutPath).absoluteFilePath()).toStdWString();
            status = persist->Load(input.c_str(), STGM_READ);
            persist->Release();
        }
        if (SUCCEEDED(status)) {
            std::vector<wchar_t> pathBuffer(32768u, L'\0');
            std::vector<wchar_t> argumentBuffer(32768u, L'\0');
            WIN32_FIND_DATAW data{};
            status = link->GetPath(pathBuffer.data(), static_cast<int>(pathBuffer.size()),
                                   &data, SLGP_RAWPATH);
            if (SUCCEEDED(status)) {
                status = link->GetArguments(argumentBuffer.data(),
                                            static_cast<int>(argumentBuffer.size()));
            }
            if (SUCCEEDED(status)) {
                *target = QString::fromWCharArray(pathBuffer.data());
                *arguments = QString::fromWCharArray(argumentBuffer.data());
                if (iconPath) {
                    std::vector<wchar_t> iconBuffer(32768u, L'\0'); int index = 0;
                    status = link->GetIconLocation(iconBuffer.data(), static_cast<int>(iconBuffer.size()), &index);
                    if (SUCCEEDED(status)) *iconPath = QString::fromWCharArray(iconBuffer.data());
                }
            }
        }
        link->Release();
    }
    if (uninitialize) CoUninitialize();
    if (FAILED(status)) {
        if (error != nullptr) *error = hresultText(status);
        return false;
    }
    return true;
}

}  // namespace Ucal::Manager
