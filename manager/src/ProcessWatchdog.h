#pragma once

#include <functional>
#include <QtGlobal>

class QProcess;

namespace Ucal::Manager {

bool activateOwnedApplicationWindow(qint64 processId);

int boundedHelperTimeoutMilliseconds(const char* environmentName, int defaultMilliseconds);
void armProcessWatchdog(QProcess* process, int timeoutMilliseconds,
                        std::function<void()> onTimeout = {});

}  // namespace Ucal::Manager
