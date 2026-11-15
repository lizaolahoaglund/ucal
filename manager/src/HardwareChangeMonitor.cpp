#include "HardwareChangeMonitor.h"
#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbt.h>
#endif

namespace Ucal::Manager {

HardwareChangeMonitor::HardwareChangeMonitor(QObject* parent, int debounceMs, int settleMs)
    : QObject(parent) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(debounceMs);
    settle_.setSingleShot(true);
    settle_.setInterval(settleMs);
    connect(&debounce_, &QTimer::timeout, this, &HardwareChangeMonitor::refreshRequested);
    connect(&settle_, &QTimer::timeout, this, &HardwareChangeMonitor::refreshRequested);
#ifdef Q_OS_WIN
    if (auto* application = QCoreApplication::instance()) application->installNativeEventFilter(this);
#endif
}

HardwareChangeMonitor::~HardwareChangeMonitor() {
#ifdef Q_OS_WIN
    if (auto* application = QCoreApplication::instance()) application->removeNativeEventFilter(this);
#endif
}

void HardwareChangeMonitor::observeChange() {
    ++generation_;
    emit changeObserved();
    // A composite USB device generates several notifications, and its runtime
    // may become ready after the first scan. Coalesce the burst and scan again
    // once after settling, without permanent polling or opening bus channels.
    debounce_.start();
    settle_.start();
}

bool HardwareChangeMonitor::nativeEventFilter(const QByteArray& eventType, void* message, long* result) {
    Q_UNUSED(result);
#ifdef Q_OS_WIN
    if ((eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") && message) {
        const auto* notification = static_cast<const MSG*>(message);
        if (notification->message == WM_DEVICECHANGE &&
            (notification->wParam == DBT_DEVNODES_CHANGED ||
             notification->wParam == DBT_DEVICEARRIVAL ||
             notification->wParam == DBT_DEVICEREMOVECOMPLETE)) observeChange();
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
#endif
    // Observing a notification must not consume Windows/Qt device processing.
    return false;
}

}  // namespace Ucal::Manager
