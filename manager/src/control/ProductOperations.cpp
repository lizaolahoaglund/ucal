#include "ProductOperations.h"
#include "BackendPlugins.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QMap>
#include <QtEndian>
#include <QVersionNumber>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <algorithm>

namespace Ucal::Manager {
namespace {

constexpr qint64 maximumManifestBytes = 64 * 1024;


}  // namespace

QString runtimeLibraryArchitecture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto dos = file.read(64);
    if (dos.size() != 64 || !dos.startsWith("MZ")) return {};
    const auto offset = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(dos.constData() + 60));
    if (offset < 64 || offset > file.size() - 24 || !file.seek(offset)) return {};
    const auto header = file.read(24);
    if (header.size() != 24 || header.left(4) != QByteArray("PE\0\0", 4)) return {};
    const auto machine = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(header.constData() + 4));
    const auto characteristics = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(header.constData() + 22));
    if ((characteristics & 0x2000) == 0) return {};
    return machine == 0x8664 ? QStringLiteral("x64") : machine == 0x14c ? QStringLiteral("x86") : QString();
}

QString runtimeFileVersion(const QString& path) {
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(native.c_str(), &ignored);
    if (size == 0 || size > 1024 * 1024) return {};
    QByteArray bytes(static_cast<int>(size), '\0');
    if (!GetFileVersionInfoW(native.c_str(), 0, size, bytes.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bdu) return {};
    return QStringLiteral("%1.%2.%3.%4").arg(HIWORD(info->dwFileVersionMS))
        .arg(LOWORD(info->dwFileVersionMS)).arg(HIWORD(info->dwFileVersionLS))
        .arg(LOWORD(info->dwFileVersionLS));
#else
    Q_UNUSED(path);
    return {};
#endif
}

QString backendDirectory(const QString& applicationDirectory) {
    const auto configured=qEnvironmentVariable("UCAL_BACKEND_DIRECTORY");
    if(!configured.isEmpty()&&QFileInfo(configured).isAbsolute())return QDir::cleanPath(configured);
    QDir directory(applicationDirectory);
    // Portable packages use bin/UcalManager.exe and Backends/ beside bin/.
    // Walking upwards also supports an uninstalled developer build.
    for (int depth = 0; depth < 8; ++depth) {
        const QString path = directory.filePath(QStringLiteral("backends"));
        if (QFileInfo(path).isDir()) return path;
        if (!directory.cdUp()) break;
    }
    return QDir(applicationDirectory).filePath(QStringLiteral("../backends"));
}

BackendDirectoryScan scanBackendDirectory(const QString& directory) {
    BackendDirectoryScan scan;
    const QFileInfoList folders = QDir(directory).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& folder : folders) {
        const QString path = QDir(folder.absoluteFilePath()).filePath(QStringLiteral("backend.json"));
        QFile file(path);
        QString error;
        QJsonObject object;
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maximumManifestBytes) {
            error = QStringLiteral("missing, unreadable or oversized backend.json");
        } else {
            QJsonParseError parse{};
            const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject())
                error = QStringLiteral("invalid JSON");
            else object = document.object();
        }
        const QString family = object.value(QStringLiteral("backendFamily")).toString();
        const QString id = object.value(QStringLiteral("id")).toString();
        if(error.isEmpty()&&(object.value(QStringLiteral("schema")).toString()==QStringLiteral("org.ucal.backend-package/2") ||
                            object.value(QStringLiteral("schema")).toString()==QStringLiteral("org.ucal.backend-package/3") ||
                            object.value(QStringLiteral("schema")).toString()==QStringLiteral("org.ucal.backend-package/4") ||
                            object.value(QStringLiteral("schema")).toString()==QStringLiteral("org.ucal.backend-package/5"))){
            BackendPackage plugin;
            if(std::any_of(scan.packages.cbegin(),scan.packages.cend(),[&](const auto& p){return p.id==id;}))error=QStringLiteral("Duplicate backend instance ID");
            else if(parseBackendPlugin(object,path,plugin,error)){scan.packages<<plugin;continue;}
            scan.diagnostics<<path+QStringLiteral(": ")+error;continue;
        }
        scan.diagnostics << path + QStringLiteral(": A concrete version 2, 3 or 4 DLL package is required");
    }
    return scan;
}

}  // namespace Ucal::Manager
