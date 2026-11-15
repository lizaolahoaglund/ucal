#include "ProcessWatchdog.h"

#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QVariant>
#include <QtGlobal>

#include <algorithm>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <set>
#include <map>
#include <vector>
#endif

namespace Ucal::Manager {

#ifdef Q_OS_WIN
namespace {
quint64 processBirthTime(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    return ok ? (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
}
struct WindowSearch { std::map<DWORD, quint64> owners; HWND window = nullptr; };
BOOL CALLBACK findOwnedWindow(HWND window, LPARAM parameter) {
    auto& found = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    const auto identity = found.owners.find(owner);
    if (identity == found.owners.end() || processBirthTime(owner) != identity->second || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr || GetWindowTextLengthW(window) == 0) return TRUE;
    found.window = GetLastActivePopup(window);
    return FALSE;
}
}
#endif

bool activateOwnedApplicationWindow(const qint64 processId) {
#ifdef Q_OS_WIN
    WindowSearch search;
    const auto rootBorn = processBirthTime(static_cast<DWORD>(processId));
    if (rootBorn == 0) return false;
    search.owners.emplace(static_cast<DWORD>(processId), rootBorn);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        std::vector<PROCESSENTRY32W> entries;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) do { entries.push_back(entry); }
            while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        bool added;
        do {
            added = false;
            for (const auto& child : entries)
                if (search.owners.count(child.th32ParentProcessID) && !search.owners.count(child.th32ProcessID)) {
                    const auto born = processBirthTime(child.th32ProcessID);
                    if (born >= rootBorn) {
                        search.owners.emplace(child.th32ProcessID, born);
                        added = true;
                    }
                }
        } while (added);
    }
    EnumWindows(findOwnedWindow, reinterpret_cast<LPARAM>(&search));
    if (search.window != nullptr) {
        ShowWindow(search.window, IsIconic(search.window) ? SW_RESTORE : SW_SHOW);
        return SetForegroundWindow(search.window) != FALSE;
    }
#endif
    return false;
}

int boundedHelperTimeoutMilliseconds(const char* const environmentName,
                                     const int defaultMilliseconds) {
    bool ok = false;
    const int configured = qEnvironmentVariableIntValue(environmentName, &ok);
    const int selected = ok ? configured : defaultMilliseconds;
    return (std::max)(100, (std::min)(selected, 300000));
}

void armProcessWatchdog(QProcess* const process, const int timeoutMilliseconds,
                        std::function<void()> onTimeout) {
    if (process == nullptr) return;
    const QPointer<QProcess> guarded(process);
    QTimer::singleShot((std::max)(100, timeoutMilliseconds), process,
                       [guarded, onTimeout = std::move(onTimeout)]() {
        if (guarded.isNull() || guarded->state() == QProcess::NotRunning) return;
        guarded->setProperty("ucalTimedOut", true);
        if (onTimeout) onTimeout();
        guarded->kill();
    });
}

}  // namespace Ucal::Manager
