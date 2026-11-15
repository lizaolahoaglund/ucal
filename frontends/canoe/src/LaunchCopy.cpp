#include "LaunchCopy.h"
#include "CanoeText.h"
#include "PeForwarder.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <windows.h>

namespace Ucal::Manager {
namespace {
struct Architecture {
    QString name, exec, native, origin, provider, providerData, data, discovery, discoveryTarget, selector;
    quint16 machine;
};
QVector<Architecture> architectures() {
    return {{"x86", "Exec32", "vcndrvms.dll", "vcndrvms_origin.dll", "UcalCanoeProvider.dll", "UcalCanoeProviderData.dll",
             "UcalVcndrvmsDataIntercept.dll", "UcalXlDiscovery.dll", "UcalVcndrvms32Discovery.dll", "CANdrv32.ini", IMAGE_FILE_MACHINE_I386},
            {"x64", "Exec64", "vcndrvms64.dll", "vcndrvms64_origin.dll", "UcalCanoeProvider64.dll", "UcalCanoeProviderData64.dll",
             "UcalVcndrvmsDataIntercept64.dll", "UcalXlDiscovery64.dll", "UcalVcndrvms64Discovery.dll", "CANdrv64.ini", IMAGE_FILE_MACHINE_AMD64}};
}
QStringList requiredRuntimeFiles() {
    return {"vcustom.ini", "Exec64/CANoe64.exe", "Exec64/CANoe01.dll", "Exec64/Vector.CANalyzer.CommonDialogs.dll",
        "Exec32/RuntimeKernel.exe", "Exec32/RTMain.dll", "Exec32/RTFrameworks.dll"};
}
bool fail(QString* error, const QString& detail) { if (error) *error = detail; return false; }
bool validInstallationId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^canoe:[A-Za-z0-9._-]{1,127}$"));
    return pattern.match(id).hasMatch();
}
QString legacyCanoeLaunchCopyPath(const QString& local, QString id) {
    id.replace(QLatin1Char(':'), QLatin1Char('-'));
    return QDir(local).filePath(QStringLiteral("UCAL/canoe-shadow/") + id);
}

QJsonObject readLaunchCopyManifest(const QString& root) {
    if (root.isEmpty()) return {};
    auto path = QDir(root).filePath(".ucal_canoe_shadow_manifest.json");
    if (!QFileInfo(path).isFile()) path = QDir(root).filePath(".ucal-canoe-shadow-manifest.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QString installRoot(const LaunchPlan& plan) {
    return QFileInfo(QFileInfo(plan.softwareExecutable).absolutePath()).absolutePath();
}
bool readImage(const QString& path, quint16 machine, QByteArray* bytes, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024 * 1024) return fail(error, path);
    *bytes = file.readAll();
    PeExportTable exports;
    if (file.error() != QFile::NoError || !readPeExports(*bytes, &exports, error)) {
        if (error) *error = path + ": " + *error;
        return false;
    }
    return exports.machine == machine || fail(error, path + QStringLiteral(": architecture mismatch"));
}
bool write(const QString& path, const QByteArray& bytes, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return fail(error, path);
    return true;
}
bool copyFile(const QString& source, const QString& target, QString* error) {
    if (QFileInfo(source).isSymLink() || !QFile::copy(source, target)) return fail(error, source + " -> " + target);
    return true;
}
bool copyDirectory(const QString& source, const QString& target, QString* error,
    const std::function<bool()>& proceed) {
    if (QFileInfo(source).isSymLink() || !QDir().mkpath(target)) return fail(error, source);
    for (const auto& entry : QDir(source).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (proceed && !proceed()) return fail(error, QStringLiteral("Preparation cancelled"));
        const auto to = QDir(target).filePath(entry.fileName());
        if (entry.isSymLink()) return fail(error, entry.absoluteFilePath());
        if (entry.isDir()) { if (!copyDirectory(entry.absoluteFilePath(), to, error, proceed)) return false; }
        else if (!entry.isFile() || !copyFile(entry.absoluteFilePath(), to, error)) return false;
    }
    return true;
}
struct Inputs { Architecture architecture; QByteArray origin, provider, data, discovery; QString package; };
bool inspect(const LaunchPlan& plan, const QString& product, const CanoeRuntimeSources& runtimes,
    QVector<Inputs>* inputs, QString* error) {
    if (error) error->clear();
    inputs->clear();
    if (!validInstallationId(plan.softwareInstallationId) ||
        (plan.integrationAdapterId != "canoe-provider" && plan.integrationAdapterId != "vxl-process-route") ||
        QFileInfo(plan.softwareExecutable).fileName().compare("CANoe64.exe", Qt::CaseInsensitive) != 0 ||
        !QFileInfo(plan.softwareExecutable).isAbsolute()) return fail(error, QStringLiteral("Invalid CANoe installation selection"));
    const auto root = installRoot(plan);
    for (const auto& relative : requiredRuntimeFiles()) {
        const auto path = QDir(root).filePath(relative);
        if (!QFileInfo(path).isFile()) return fail(error, path);
    }
    for (const auto& architecture : architectures()) {
        Inputs item; item.architecture = architecture;
        item.package = QDir(product).filePath("frontends/canoe/" + architecture.name);
        const auto native = architecture.name == "x86" ? runtimes.x86 : runtimes.x64;
        if (!readImage(native, architecture.machine, &item.origin, error) ||
            !readImage(QDir(item.package).filePath(architecture.provider), architecture.machine, &item.provider, error) ||
            !readImage(QDir(item.package).filePath(architecture.data), architecture.machine, &item.data, error) ||
            !readImage(QDir(product).filePath("frontends/vxlapi/" + architecture.name + '/' + architecture.discovery),
                architecture.machine, &item.discovery, error)) return false;
        for (const auto& data : {item.provider, item.data}) {
            if (makeCanoeForwarder(item.origin, data, architecture.machine, architecture.provider.toLatin1(),
                    "origin", "data", error).isEmpty()) return false;
        }
        inputs->push_back(item);
    }
    return true;
}
}

CanoeRuntimeSources installedCanoeRuntimes() {
    wchar_t directory[MAX_PATH + 1]{};
    const auto count = GetWindowsDirectoryW(directory, MAX_PATH);
    if (count == 0 || count >= MAX_PATH) return {};
    const auto windows = QDir(QString::fromWCharArray(directory, int(count)));
    QString native = "System32";
#ifndef _WIN64
    BOOL wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64) native = "Sysnative";
#endif
    return {windows.filePath("SysWOW64/vcndrvms.dll"), windows.filePath(native + "/vcndrvms64.dll")};
}

QString canoeDriverRegistrationError(const UiLanguage language, const QString& registryRoot) {
    // The native x86 and x64 runtimes share KEY_WOW64_64KEY registration.
    // Preserve access failures instead of treating every empty read as absence.
    HKEY hive = nullptr;
    QString subKey;
    for (const auto& prefix : {QStringLiteral("HKEY_LOCAL_MACHINE\\"),
                              QStringLiteral("HKEY_CURRENT_USER\\")}) {
        if (registryRoot.startsWith(prefix, Qt::CaseInsensitive)) {
            hive = prefix.startsWith(QStringLiteral("HKEY_LOCAL_MACHINE"))
                ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
            subKey = registryRoot.mid(prefix.size());
            break;
        }
    }
    HKEY registration = nullptr;
    auto status = hive && !subKey.isEmpty()
        ? RegOpenKeyExW(hive, reinterpret_cast<LPCWSTR>(subKey.utf16()), 0,
            KEY_READ | KEY_WOW64_64KEY, &registration)
        : ERROR_INVALID_PARAMETER;
    DWORD drivers = 0;
    const bool missing = status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
    if (status == ERROR_SUCCESS) {
        status = RegQueryInfoKeyW(registration, nullptr, nullptr, nullptr, &drivers,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(registration);
    }
    if (!missing && status != ERROR_SUCCESS)
        return canoeProfileText(language, "launch.blocker.canoeDriverRegistryUnreadable")
            .arg(registryRoot);
    if (missing || drivers == 0)
        return canoeProfileText(language, "launch.blocker.canoeDriverRegistryMissing")
            .arg(registryRoot, registryRoot + QStringLiteral("\\VN1610"));
    // Other registered devices (including Virtual CAN) are legitimate carriers.
    // A populated directory is not a claim that physical channels are available.
    return {};
}

QString canoeLaunchCopyPath(const QString& softwareInstallationId, const QString& version) {
    if (!validInstallationId(softwareInstallationId)) return {};
    const auto local = QFileInfo(qEnvironmentVariable("LOCALAPPDATA")).canonicalFilePath();
    if (local.isEmpty() || !QFileInfo(local).isDir()) return {};
    const auto legacy = legacyCanoeLaunchCopyPath(local, softwareInstallationId);
    // CANoe 11 and 12 aborted from a longer copied runtime path.
    // Keep the existing layout within the observed working path length.
    constexpr int legacyPathBudget = 144;
    const auto major = version.section('.', 0, 0);
    if ((major != QStringLiteral("11") && major != QStringLiteral("12")) ||
        legacy.size() <= legacyPathBudget) return legacy;
    const auto key = QCryptographicHash::hash(softwareInstallationId.toUtf8(), QCryptographicHash::Sha256).toHex().left(20);
    return QDir(local).filePath(QStringLiteral("UCAL/c/") + QString::fromLatin1(key));
}

bool completeCanoeLaunchCopy(const QString& root) {
    if (!readLaunchCopyManifest(root).value("completeRuntime").toBool()) return false;
    for (const auto& relative : requiredRuntimeFiles()) if (!QFileInfo(QDir(root).filePath(relative)).isFile()) return false;
    return true;
}

bool inspectCanoeLaunchCopyInputs(const LaunchPlan& plan, const QString& product,
    const CanoeRuntimeSources& runtimes, QString* error) {
    QVector<Inputs> inputs;
    return inspect(plan, product, runtimes, &inputs, error);
}

bool provisionCanoeLaunchCopy(const LaunchPlan& plan, const QString& product,
    const CanoeRuntimeSources& runtimes, QString* error, const std::function<bool()>& proceed) {
    QVector<Inputs> inputs;
    if (!inspect(plan, product, runtimes, &inputs, error)) return false;
    const auto local = QFileInfo(qEnvironmentVariable("LOCALAPPDATA")).canonicalFilePath();
    if (local.isEmpty() || !QFileInfo(local).isDir()) return fail(error, QStringLiteral("Local application data directory is unavailable"));
    const auto target = canoeLaunchCopyPath(plan.softwareInstallationId, plan.softwareVersion);
    if (target.isEmpty()) return fail(error, plan.softwareInstallationId);
    const QFileInfo targetInfo(target);
    const auto cache = targetInfo.absolutePath();
    const auto id = targetInfo.fileName();
    const auto source = QFileInfo(installRoot(plan)).canonicalFilePath();
    // Resolve existing ancestors before mkdir: a redirected cache must not make
    // even an empty directory inside the vendor installation or Windows.
    auto ancestor = cache;
    while (!QFileInfo(ancestor).exists()) {
        const auto parent = QFileInfo(ancestor).absolutePath();
        if (parent == ancestor) return fail(error, cache);
        ancestor = parent;
    }
    const auto resolvedCache = QDir::cleanPath(QDir(QFileInfo(ancestor).canonicalFilePath()).filePath(QDir(ancestor).relativeFilePath(cache)));
    wchar_t windowsDirectory[MAX_PATH + 1]{};
    const auto count = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    const auto windows = count && count < MAX_PATH ? QFileInfo(QString::fromWCharArray(windowsDirectory, int(count))).canonicalFilePath() : QString();
    const auto inside = [](const QString& path, const QString& parent) {
        return !parent.isEmpty() && (path.compare(parent, Qt::CaseInsensitive) == 0 || path.startsWith(parent + '/', Qt::CaseInsensitive));
    };
    if (source.isEmpty() || windows.isEmpty() || !inside(resolvedCache, local) ||
        inside(resolvedCache, source) || inside(resolvedCache, windows) || inside(source, QDir(resolvedCache).filePath(id)))
        return fail(error, cache);
    if (!QDir().mkpath(cache) || !QFileInfo(cache).canonicalFilePath().startsWith(local + '/', Qt::CaseInsensitive)) return fail(error, cache);
    const auto lockPath = QDir(cache).filePath(id + ".prepare.lock");
    QLockFile lock(lockPath);
    if (!lock.tryLock(0)) return fail(error, lockPath);
    if (QFileInfo(target).exists() && (QFileInfo(target).isSymLink() ||
        !QFileInfo(target).canonicalFilePath().startsWith(QFileInfo(cache).canonicalFilePath() + '/', Qt::CaseInsensitive)))
        return fail(error, target);
    if (completeCanoeLaunchCopy(target)) return true;
    // The temporary directory owns only this attempt's files; an existing copy
    // and its settings are never overlaid or removed on a failed preparation.
    QTemporaryDir staging(QDir(cache).filePath(".new-" + id + "-XXXXXX"));
    if (!staging.isValid()) return fail(error, cache);
    for (const auto& directory : {QStringLiteral("Exec32"), QStringLiteral("Exec64")})
        if (!copyDirectory(QDir(source).filePath(directory), QDir(staging.path()).filePath(directory), error, proceed)) return false;
    auto customization = QDir(source).filePath("vcustom.ini");
    const auto legacy = legacyCanoeLaunchCopyPath(local, plan.softwareInstallationId);
    if (legacy != target &&
        QFileInfo(legacy).canonicalFilePath().compare(legacy, Qt::CaseInsensitive) == 0) {
        const auto manifest = readLaunchCopyManifest(legacy);
        const auto recordedRoot = manifest.value("installRoot").toString();
        const auto recordedId = manifest.value("softwareInstallationId").toString();
        // Older engineering manifests did not record the installation ID.
        // They still record the source installation and use the same keyed path.
        if (manifest.value("schema").toString() == QStringLiteral("org.ucal.canoe-shadow/1") &&
            manifest.value("completeRuntime").toBool() &&
            QFileInfo(recordedRoot).canonicalFilePath().compare(source, Qt::CaseInsensitive) == 0 &&
            (recordedId.isEmpty() || recordedId == plan.softwareInstallationId)) {
            const auto priorSettings = QDir(legacy).filePath("vcustom.ini");
            const QFileInfo priorInfo(priorSettings);
            if (priorInfo.exists() || priorInfo.isSymLink()) {
                if (priorInfo.canonicalFilePath().compare(priorSettings, Qt::CaseInsensitive) != 0)
                    return fail(error, priorSettings);
                customization = priorSettings;
            }
        }
    }
    if (!copyFile(customization, QDir(staging.path()).filePath("vcustom.ini"), error)) return false;
    for (const auto& item : inputs) {
        if (proceed && !proceed()) return fail(error, QStringLiteral("Preparation cancelled"));
        const auto& a = item.architecture;
        const auto exec = QDir(staging.path()).filePath(a.exec);
        if (!QDir().mkpath(QDir(exec).filePath("CANdrv"))) return fail(error, exec);
        const auto originModule = QFileInfo(a.origin).completeBaseName().toLatin1();
        const auto provider = makeCanoeForwarder(item.origin, item.provider, a.machine, a.provider.toLatin1(),
            originModule, QFileInfo(a.providerData).completeBaseName().toLatin1(), error);
        const auto native = makeCanoeForwarder(item.origin, item.data, a.machine, a.native.toLatin1(),
            originModule, QFileInfo(a.data).completeBaseName().toLatin1(), error);
        if (provider.isEmpty() || native.isEmpty()) return false;
        const QByteArray selector = "[CAN Driver]\r\nName=" + a.provider.toLatin1() + "\r\n";
        const QVector<QPair<QString, QByteArray>> assets{
            {a.origin, item.origin}, {a.provider, provider}, {a.providerData, item.provider},
            {a.native, native}, {a.data, item.data}, {a.discovery, item.discovery}, {a.discoveryTarget, item.discovery},
            {a.selector, selector}, {"CANdrv/UCAL.ini", selector}, {"ucal_vcndrvms.enabled", "1\n"}};
        for (const auto& asset : assets) if (!write(QDir(exec).filePath(asset.first), asset.second, error)) return false;
        for (const auto& runtime : {QStringLiteral("MSVCP140.dll"), QStringLiteral("VCRUNTIME140.dll"), QStringLiteral("VCRUNTIME140_1.dll")}) {
            const auto path = QDir(item.package).filePath(runtime);
            if (QFileInfo(path).isFile()) {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) return fail(error, path);
                const auto bytes = file.readAll();
                if (file.error() != QFile::NoError || bytes.isEmpty() || !write(QDir(exec).filePath(runtime), bytes, error)) return false;
            }
        }
    }
    const QJsonObject provider{{"schema", "org.ucal.canoe-provider-shadow/1"},
        {"activationScope", "ucal_owned_isolated_shadow"}, {"installedVectorFilesModified", false}};
    const QJsonObject manifest{{"schema", "org.ucal.canoe-shadow/1"}, {"completeRuntime", true},
        {"installRoot", source}, {"softwareInstallationId", plan.softwareInstallationId}, {"nativeVectorFilesModified", false}};
    if (!write(QDir(staging.path()).filePath(".ucal_provider_shadow.json"), QJsonDocument(provider).toJson(), error) ||
        !write(QDir(staging.path()).filePath(".ucal_canoe_shadow_manifest.json"), QJsonDocument(manifest).toJson(), error)) return false;
    const auto previous = target + ".previous-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool existed = QFileInfo(target).exists();
    if (existed && !QDir().rename(target, previous)) return fail(error, target);
    if (!QDir().rename(staging.path(), target)) {
        if (existed && !QDir().rename(previous, target)) return fail(error, previous + QStringLiteral(": restore failed"));
        return fail(error, target);
    }
    staging.setAutoRemove(false);
    return true;
}
} // namespace Ucal::Manager
