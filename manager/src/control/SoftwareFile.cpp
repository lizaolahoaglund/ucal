#include "SoftwareDiscovery.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <windows.h>
#include <winver.h>
#include <vector>
namespace Ucal::Manager {
QString executableVersion(const QString& path) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(native.c_str(), &ignored);
    if (bytes == 0) return QString();
    std::vector<std::byte> buffer(bytes);
    if (!GetFileVersionInfoW(native.c_str(), 0, bytes, buffer.data())) return QString();
    VS_FIXEDFILEINFO* information = nullptr;
    UINT informationBytes = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&information),
                        &informationBytes) || information == nullptr ||
        informationBytes < sizeof(VS_FIXEDFILEINFO)) {
        return QString();
    }
    return QStringLiteral("%1.%2.%3.%4")
        .arg(HIWORD(information->dwFileVersionMS))
        .arg(LOWORD(information->dwFileVersionMS))
        .arg(HIWORD(information->dwFileVersionLS))
        .arg(LOWORD(information->dwFileVersionLS));
}

QString executableArchitecture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 64) return QStringLiteral("Unknown");
    const QByteArray dos = file.read(64);
    if (dos.size() != 64 || dos.at(0) != 'M' || dos.at(1) != 'Z') return QStringLiteral("Unknown");
    const auto byte = [&dos](const int offset) {
        return static_cast<quint32>(static_cast<unsigned char>(dos.at(offset)));
    };
    const quint32 peOffset = byte(0x3c) | (byte(0x3d) << 8u) |
                             (byte(0x3e) << 16u) | (byte(0x3f) << 24u);
    if (!file.seek(peOffset)) return QStringLiteral("Unknown");
    const QByteArray header = file.read(6);
    if (header.size() != 6 || header.left(4) != QByteArray("PE\0\0", 4)) {
        return QStringLiteral("Unknown");
    }
    const quint16 machine = static_cast<quint16>(static_cast<unsigned char>(header.at(4))) |
                            static_cast<quint16>(static_cast<unsigned char>(header.at(5)) << 8u);
    if (machine == IMAGE_FILE_MACHINE_AMD64) return QStringLiteral("x64");
    if (machine == IMAGE_FILE_MACHINE_I386) return QStringLiteral("x86");
    if (machine == 0xaa64u) return QStringLiteral("ARM64");
    return QStringLiteral("Unknown");
}

QString stableSoftwareId(const QString& family, const QString& path) {
    const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
    const QByteArray digest = QCryptographicHash::hash(normalized.toUtf8(),
                                                       QCryptographicHash::Sha256).toHex();
    return QStringLiteral("%1:%2").arg(family, QString::fromLatin1(digest.left(20)));
}

}
