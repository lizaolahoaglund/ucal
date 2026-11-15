#include "CanoeText.h"
#include "Profile.h"
#include "LaunchCopy.h"
#include "ProductLauncher.h"
#include "FrontendPlugins.h"
#include <QJsonArray>
#include <QHash>
#include <QRegularExpression>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace Ucal::Manager {
namespace {
bool requireFiles(const QStringList& files, QString* missing) {
    for (const QString& file : files) {
        if (!QFileInfo(file).isFile()) {
            if (missing != nullptr) *missing = file;
            return false;
        }
    }
    return true;
}
QString backendKey(const QStringList& families) {
    if (families.size() != 1) return QStringLiteral("ALL");
    return families.first().section(QRegularExpression(QStringLiteral("[ -]")), 0, 0).toUpper();
}
QString productAsset(const QString& root, const QString& name) {
    const auto current = QDir(root).filePath(name);
    if (QFileInfo(current).isFile()) return current;
    // Existing user-owned launch copies are persisted state, independent of
    // the spelling used for newly built source artifacts.
    const QHash<QString, QStringList> legacyNames{
        {QStringLiteral(".ucal_canoe_shadow_manifest.json"), {QStringLiteral(".ucal-canoe-shadow-manifest.json")}},
        {QStringLiteral(".ucal_provider_shadow.json"), {QStringLiteral(".ucal-provider-shadow.json")}},
        {QStringLiteral("UcalCanoeProvider.dll"), {QStringLiteral("ucal_canoe_provider.dll"), QStringLiteral("ucal-canoe-provider.dll")}},
        {QStringLiteral("UcalCanoeProvider64.dll"), {QStringLiteral("ucal_canoe_provider64.dll"), QStringLiteral("ucal-canoe-provider64.dll")}},
        {QStringLiteral("UcalCanoeProviderData.dll"), {QStringLiteral("ucal_canoe_provider_data.dll"), QStringLiteral("ucal-canoe-provider-data.dll")}},
        {QStringLiteral("UcalCanoeProviderData64.dll"), {QStringLiteral("ucal_canoe_provider_data64.dll"), QStringLiteral("ucal-canoe-provider-data64.dll")}},
        {QStringLiteral("UcalXlDiscovery.dll"), {QStringLiteral("ucal_xl_discovery.dll")}},
        {QStringLiteral("UcalXlDiscovery64.dll"), {QStringLiteral("ucal_xl_discovery64.dll")}}};
    for (const auto& legacy : legacyNames.value(name)) {
        const auto path=QDir(root).filePath(legacy);
        if (QFileInfo(path).isFile()) return path;
    }
    return current;
}
bool completeCanoeShadow(const QString& root) {
    return completeCanoeLaunchCopy(root);
}
ProductLaunchSpec prepareCanoe(const LaunchPlan& plan, const UiLanguage language) {
    ProductLaunchSpec spec;
    const QFileInfo executable(plan.softwareExecutable);
    const QString installExec64 = executable.absolutePath();
    const QString installRoot = QFileInfo(installExec64).absolutePath();
    const QString shadowRoot = canoeLaunchCopyPath(plan.softwareInstallationId, plan.softwareVersion);
    // Only a fully provisioned application copy may override the installed
    // route. Historical EXE-only shadows cannot resolve CANoe's .NET assemblies.
    const bool useShadow = !shadowRoot.isEmpty() && completeCanoeShadow(shadowRoot) &&
        QFileInfo(productAsset(shadowRoot, QStringLiteral(".ucal_provider_shadow.json"))).isFile();
    const QString root = useShadow ? shadowRoot : installRoot;
    const QString exec64 = QDir(root).filePath(QStringLiteral("Exec64"));
    const QString exec32 = QDir(root).filePath(QStringLiteral("Exec32"));
    QStringList required;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("UCAL_PROFILE_ID"), plan.applicationId);
    if (useShadow) {
        const QString installExec32 = QDir(installRoot).filePath(QStringLiteral("Exec32"));
        environment.insert(QStringLiteral("PATH"),
            installExec64 + QLatin1Char(';') + installExec32 + QLatin1Char(';') +
            environment.value(QStringLiteral("PATH")));
    }
    environment.insert(QStringLiteral("UCAL_BACKEND"), backendKey(plan.backendFamilies));
    environment.insert(QStringLiteral("UCAL_TX_ACK_TIMEOUT_MS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("UCAL_QUEUED_TX_ACK_TIMEOUT_MS"), QStringLiteral("50"));
    environment.insert(QStringLiteral("UCAL_VCNDRVMS_BACKEND"), QStringLiteral("1"));
    environment.insert(QStringLiteral("UCAL_CANOE_HARDWARE_PRESENTATION"), QStringLiteral("1"));
    environment.insert(QStringLiteral("UCAL_CANOE_TIMING_TARGETS"), QStringLiteral("1"));
    environment.insert(QStringLiteral("UCAL_VECTOR_CAN_ONLY"), QStringLiteral("1"));
    QStringList endpoints;
    for (const LaunchEndpoint& endpoint : plan.endpoints) endpoints.push_back(endpoint.endpointId);
    environment.insert(QStringLiteral("UCAL_VCNDRVMS_CAN_ENDPOINTS"),
                       endpoints.join(QLatin1Char(';')));

    if (plan.integrationAdapterId == QStringLiteral("canoe-provider")) {
        required = QStringList{
            productAsset(root, QStringLiteral(".ucal_provider_shadow.json")),
            productAsset(exec32, QStringLiteral("UcalCanoeProvider.dll")),
            productAsset(exec64, QStringLiteral("UcalCanoeProvider64.dll")),
            productAsset(exec32, QStringLiteral("UcalCanoeProviderData.dll")),
            productAsset(exec64, QStringLiteral("UcalCanoeProviderData64.dll")),
            productAsset(exec32, QStringLiteral("UcalXlDiscovery.dll")),
            productAsset(exec64, QStringLiteral("UcalXlDiscovery64.dll")),
            QDir(exec32).filePath(QStringLiteral("CANdrv32.ini")),
            QDir(exec64).filePath(QStringLiteral("CANdrv64.ini"))};
        environment.insert(QStringLiteral("UCAL_XL_DISCOVERY_DLL32"), required.at(5));
        environment.insert(QStringLiteral("UCAL_XL_DISCOVERY_DLL64"), required.at(6));
    } else if (plan.integrationAdapterId == QStringLiteral("vxl-process-route")) {
        required = QStringList{
            QDir(exec32).filePath(QStringLiteral("vcndrvms.dll")),
            QDir(exec64).filePath(QStringLiteral("vcndrvms64.dll")),
            QDir(exec32).filePath(QStringLiteral("vcndrvms_origin.dll")),
            QDir(exec64).filePath(QStringLiteral("vcndrvms64_origin.dll")),
            QDir(exec32).filePath(QStringLiteral("UcalVcndrvmsDataIntercept.dll")),
            QDir(exec64).filePath(QStringLiteral("UcalVcndrvmsDataIntercept64.dll")),
            QDir(exec32).filePath(QStringLiteral("UcalVcndrvms32Discovery.dll")),
            QDir(exec64).filePath(QStringLiteral("UcalVcndrvms64Discovery.dll")),
            QDir(exec32).filePath(QStringLiteral("ucal_vcndrvms.enabled"))};
        environment.insert(QStringLiteral("UCAL_VCNDRVMS_DISCOVERY_DLL32"), required.at(6));
        environment.insert(QStringLiteral("UCAL_VCNDRVMS_DISCOVERY_DLL64"), required.at(7));
    } else {
        spec.error = canoeProfileText(language, QStringLiteral("launch.product.canoeAdapterUnsupported"));
        return spec;
    }

    QString missing;
    if (!requireFiles(required, &missing)) {
        spec.error = canoeProfileText(language, QStringLiteral("launch.product.canoeRouteIncomplete"))
                         .arg(QDir::toNativeSeparators(missing));
        return spec;
    }
    spec.program = useShadow
        ? QDir(exec64).filePath(QStringLiteral("CANoe64.exe"))
        : executable.absoluteFilePath();
    if (useShadow && !QFileInfo(spec.program).isFile()) {
        spec.error = canoeProfileText(language, QStringLiteral("launch.product.canoeShadowIncomplete"))
                         .arg(QDir::toNativeSeparators(spec.program));
        return spec;
    }
    spec.workingDirectory = exec64;
    if (!plan.configurationPath.trimmed().isEmpty()) {
        if (!QFileInfo(plan.configurationPath).isFile()) {
            spec.error = canoeProfileText(language, QStringLiteral("launch.configurationMissing"));
            return spec;
        }
    }
    spec.arguments = applicationArguments(plan);
    spec.environment = environment;
    return spec;
}
}
QString preferredProfileAdapter(const SoftwareInstallation& software) {
    if (software.family == QStringLiteral("canoe")) {
        const QString shadow = canoeLaunchCopyPath(software.id, software.version);
        const bool staged = completeCanoeShadow(shadow);
        const QString root = staged ? shadow : QFileInfo(QFileInfo(software.executablePath).absolutePath()).absolutePath();
        QStringList processAssets;
        for (const QString& relative : {
                 QStringLiteral("Exec32/vcndrvms.dll"), QStringLiteral("Exec64/vcndrvms64.dll"),
                 QStringLiteral("Exec32/vcndrvms_origin.dll"), QStringLiteral("Exec64/vcndrvms64_origin.dll"),
                 QStringLiteral("Exec32/UcalVcndrvmsDataIntercept.dll"),
                 QStringLiteral("Exec64/UcalVcndrvmsDataIntercept64.dll"),
                 QStringLiteral("Exec32/UcalVcndrvms32Discovery.dll"),
                 QStringLiteral("Exec64/UcalVcndrvms64Discovery.dll"),
                 QStringLiteral("Exec32/ucal_vcndrvms.enabled")})
            processAssets << QDir(root).filePath(relative);
        if (software.integrationAdapters.contains(QStringLiteral("vxl-process-route")) &&
            requireFiles(processAssets, nullptr))
            return QStringLiteral("vxl-process-route");
        if (staged && software.integrationAdapters.contains(QStringLiteral("canoe-provider")) &&
            QFileInfo(productAsset(root, QStringLiteral(".ucal_provider_shadow.json"))).isFile())
            return QStringLiteral("canoe-provider");
    }
    return software.integrationAdapters.value(0);
}
ProductLaunchSpec prepareProfileLaunch(const QString& implementation, const LaunchPlan& plan,
    const QString& applicationDirectory, const QString& temporaryDirectory, UiLanguage language,
    const QSet<int>& reserved) {
    if (implementation == QStringLiteral("canoe")) return prepareCanoe(plan, language);
    ProductLaunchSpec result;
    result.error = canoeProfileText(language, QStringLiteral("launch.product.routeUnavailable"));
    return result;
}

bool synchronizeCanoeLaunchAssets(const LaunchPlan& plan, const QString& applicationDirectory,
    const UiLanguage language, ProductLaunchSpec* spec) {
    const QString root = QFileInfo(QFileInfo(spec->program).absolutePath()).absolutePath();
    const QString expectedCopy = canoeLaunchCopyPath(plan.softwareInstallationId, plan.softwareVersion);
    const QString cache = QFileInfo(expectedCopy).absolutePath();
    // Manually provisioned engineering routes remain read-only. Automatic
    // replacement belongs only to the complete Manager-owned launch copy.
    if (expectedCopy.isEmpty() || QDir::cleanPath(root).compare(expectedCopy, Qt::CaseInsensitive) != 0)
        return true;
    const auto fail = [spec, language](const QString& detail) {
        spec->error = canoeProfileText(language, QStringLiteral("launch.product.canoeAssetSync"))
            .arg(detail);
        return false;
    };
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QString canonicalCache = QFileInfo(cache).canonicalFilePath();
    const QString canonicalLocal = QFileInfo(qEnvironmentVariable("LOCALAPPDATA")).canonicalFilePath();
    if (!completeCanoeShadow(root) || canonicalLocal.isEmpty() || canonicalCache.isEmpty() || canonicalRoot.isEmpty() ||
        !canonicalCache.startsWith(canonicalLocal + QLatin1Char('/'), Qt::CaseInsensitive) ||
        !canonicalRoot.startsWith(canonicalCache + QLatin1Char('/'), Qt::CaseInsensitive))
        return fail(root);
    const QString lockPath = QDir(root).filePath(QStringLiteral(".ucal_runtime_update.lock"));
    QLockFile lock(lockPath);
    if (!lock.tryLock(0)) return fail(lockPath);

    struct Asset { QString source, target; QByteArray before, after; bool existed = false; };
    QVector<Asset> changed;
    QString error;
    const auto add = [&](const QString& source, const QString& target) {
        QFile input(source);
        if (!input.open(QIODevice::ReadOnly)) { error = source + QStringLiteral(": ") + input.errorString(); return false; }
        Asset asset;
        asset.source = source; asset.target = target; asset.after = input.readAll();
        if (input.error() != QFile::NoError || asset.after.isEmpty()) { error = source; return false; }
        asset.existed = QFileInfo(target).exists();
        const QString resolved = asset.existed ? QFileInfo(target).canonicalFilePath()
            : QFileInfo(QFileInfo(target).absolutePath()).canonicalFilePath() + QLatin1Char('/') + QFileInfo(target).fileName();
        if (!resolved.startsWith(canonicalRoot + QLatin1Char('/'), Qt::CaseInsensitive)) { error = target; return false; }
        if (asset.existed) {
            QFile old(target);
            if (!old.open(QIODevice::ReadOnly)) { error = target + QStringLiteral(": ") + old.errorString(); return false; }
            asset.before = old.readAll();
            if (old.error() != QFile::NoError) { error = target; return false; }
        }
        if (!asset.existed || asset.before != asset.after) changed.push_back(asset);
        return true;
    };
    for (const QString& architecture : {QStringLiteral("x86"), QStringLiteral("x64")}) {
        const bool x64 = architecture == QStringLiteral("x64");
        const QString exec = QDir(root).filePath(x64 ? QStringLiteral("Exec64") : QStringLiteral("Exec32"));
        const QString packaged = QDir(applicationDirectory).filePath(QStringLiteral("frontends/canoe/") + architecture);
        const QString providerSource = x64 ? QStringLiteral("UcalCanoeProvider64.dll") : QStringLiteral("UcalCanoeProvider.dll");
        const QString providerData = x64 ? QStringLiteral("UcalCanoeProviderData64.dll") : QStringLiteral("UcalCanoeProviderData.dll");
        const QString data = x64 ? QStringLiteral("UcalVcndrvmsDataIntercept64.dll") : QStringLiteral("UcalVcndrvmsDataIntercept.dll");
        const QString discovery = x64 ? QStringLiteral("UcalXlDiscovery64.dll") : QStringLiteral("UcalXlDiscovery.dll");
        const QString discoveryTarget = x64 ? QStringLiteral("UcalVcndrvms64Discovery.dll") : QStringLiteral("UcalVcndrvms32Discovery.dll");
        const QString discoverySource = QDir(applicationDirectory).filePath(
            QStringLiteral("frontends/vxlapi/") + architecture + QLatin1Char('/') + discovery);
        // Both entry shells are consumed by a complete CANoe copy, regardless
        // of the chosen route. Keep their data and discovery aliases coherent.
        if (!add(QDir(packaged).filePath(providerSource), productAsset(exec, providerData)) ||
            !add(QDir(packaged).filePath(data), productAsset(exec, data)) ||
            !add(discoverySource, productAsset(exec, discovery)) ||
            !add(discoverySource, productAsset(exec, discoveryTarget))) return fail(error);
        for (const QString& runtime : {QStringLiteral("MSVCP140.dll"), QStringLiteral("VCRUNTIME140.dll"), QStringLiteral("VCRUNTIME140_1.dll")}) {
            const QString source = QDir(packaged).filePath(runtime);
            if (QFileInfo(source).isFile() && !add(source, QDir(exec).filePath(runtime))) return fail(error);
        }
    }
    if (changed.isEmpty()) return true;
#ifdef Q_OS_WIN
    struct WriteLocks {
        QVector<HANDLE> handles;
        void release() { for (const auto handle : handles) CloseHandle(handle); handles.clear(); }
        ~WriteLocks() { release(); }
    } writes;
    // Check every changed image before replacing any file. Mapped DLLs cannot
    // be opened for writing; a running CANoe therefore keeps the complete old set.
    for (const auto& asset : changed) if (asset.existed) {
        const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(asset.target.utf16()), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return fail(asset.target + QStringLiteral(" (Win32 %1)").arg(GetLastError()));
        writes.handles.push_back(handle);
    }
#endif
    const auto write = [](const QString& path, const QByteArray& bytes) {
        QSaveFile output(path);
        return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
    };
    const QString backup = QDir(root).filePath(QStringLiteral(".ucal-runtime-backups/") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QJsonArray entries;
    for (const auto& asset : changed) {
        const QString relative = QDir(root).relativeFilePath(asset.target);
        const QString saved = QDir(backup).filePath(relative);
        if (!QDir().mkpath(QFileInfo(saved).absolutePath()) ||
            (asset.existed && !write(saved, asset.before))) return fail(saved);
        const QString staged = QDir(backup).filePath(QStringLiteral("new/") + relative);
        if (!QDir().mkpath(QFileInfo(staged).absolutePath()) || !write(staged, asset.after)) return fail(staged);
        entries.append(QJsonObject{{"path", relative}, {"existed", asset.existed},
            {"beforeSha256", QString::fromLatin1(QCryptographicHash::hash(asset.before, QCryptographicHash::Sha256).toHex())},
            {"afterSha256", QString::fromLatin1(QCryptographicHash::hash(asset.after, QCryptographicHash::Sha256).toHex())}});
    }
    const QString receipt = QDir(backup).filePath(QStringLiteral("update.json"));
    QJsonObject record{{"state", "prepared"}, {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"files", entries}};
    if (!write(receipt, QJsonDocument(record).toJson())) return fail(receipt);
    const auto replace = [](const QString& source, const QString& target) {
#ifdef Q_OS_WIN
        return MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()), reinterpret_cast<LPCWSTR>(target.utf16()),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        return std::rename(QFile::encodeName(source).constData(), QFile::encodeName(target).constData()) == 0;
#endif
    };
    // Allocate and save every old/new image before changing any target. Commit
    // and rollback then use same-volume renames, including on a full disk.
#ifdef Q_OS_WIN
    // Windows cannot replace a destination while our own write handle is open.
    // Keep the per-copy lock; release image probes only after staging is complete.
    writes.release();
#endif
    int applied = 0;
    for (const auto& asset : changed) {
        const QString relative = QDir(root).relativeFilePath(asset.target);
        if (!replace(QDir(backup).filePath(QStringLiteral("new/") + relative), asset.target)) { error = asset.target; break; }
        ++applied;
    }
    if (applied == changed.size()) {
        record.insert("state", "committed");
        if (write(receipt, QJsonDocument(record).toJson())) return true;
        error = receipt;
    }
    {
        bool restored = true;
        for (int index = applied - 1; index >= 0; --index) {
            const auto& asset = changed.at(index);
            const QString saved = QDir(backup).filePath(QDir(root).relativeFilePath(asset.target));
            if (!(asset.existed ? replace(saved, asset.target) : QFile::remove(asset.target))) {
                error += QStringLiteral("; rollback: ") + asset.target;
                restored = false;
            }
        }
        record.insert("state", restored ? "rolled-back" : "rollback-incomplete");
        write(receipt, QJsonDocument(record).toJson());
        return fail(error);
    }
}
}
