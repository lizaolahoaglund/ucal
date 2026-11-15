#pragma once
#include "LaunchPlan.h"
#include "UiPresentation.h"
#include <functional>

namespace Ucal::Manager {

struct CanoeRuntimeSources { QString x86, x64; };
CanoeRuntimeSources installedCanoeRuntimes();
// Read the same machine-wide 64-bit view used by both native runtime bitnesses.
// An explicit root permits isolated registry fixtures without changing HKLM.
QString canoeDriverRegistrationError(UiLanguage language, const QString& registryRoot =
    QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\VECTOR\\CANDriver 1.0\\Drivers"));
QString canoeLaunchCopyPath(const QString& softwareInstallationId, const QString& version);
bool completeCanoeLaunchCopy(const QString& root);
// Inspection is read-only. Explicit source paths also permit original test
// runtimes without consulting any installed vendor program or personal setting.
bool inspectCanoeLaunchCopyInputs(const LaunchPlan& plan, const QString& product,
    const CanoeRuntimeSources& runtimes, QString* error);
bool provisionCanoeLaunchCopy(const LaunchPlan& plan, const QString& product,
    const CanoeRuntimeSources& runtimes, QString* error,
    const std::function<bool()>& continuePreparation = {});

} // namespace Ucal::Manager
