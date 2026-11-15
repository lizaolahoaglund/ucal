#include "Branding.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QPainter>
#include <QSaveFile>
#include <QDateTime>
#include <QPixmap>
#include <QVector>

namespace Ucal::Manager {
namespace {
void appendU16(QByteArray& bytes, quint16 value) {
    bytes.append(char(value)); bytes.append(char(value >> 8));
}
void appendU32(QByteArray& bytes, quint32 value) {
    appendU16(bytes, quint16(value)); appendU16(bytes, quint16(value >> 16));
}
}

QIcon managerIcon() { return QIcon(QStringLiteral(":/UcalManager/ucal.png")); }

QImage composeApplicationIcon(const QImage& original, const QImage& badge, int size) {
    if (size < 16 || size > 256 || original.isNull() || badge.isNull()) return {};
    QImage result(size, size, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    const auto base = original.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    painter.drawImage(QPoint((size - base.width()) / 2, (size - base.height()) / 2), base);
    const int badgeSize = qMax(8, qRound(size * 0.42));
    const QRect target(size - badgeSize, size - badgeSize, badgeSize, badgeSize);
    painter.setPen(QPen(QColor(255, 255, 255, 235), qMax(1.0, size / 128.0)));
    painter.setBrush(QColor(255, 255, 255, 235));
    painter.drawRoundedRect(QRectF(target).adjusted(0.5, 0.5, -0.5, -0.5), badgeSize * 0.2, badgeSize * 0.2);
    painter.drawImage(target, badge);
    return result;
}

bool writeIconFile(const QString& path, const QIcon& icon, QString* error) {
    if (error) error->clear();
    const QVector<int> sizes{16, 24, 32, 48, 64, 128, 256};
    QVector<QByteArray> images;
    for (const auto size : sizes) {
        const auto image = icon.pixmap(size, size).toImage().scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QByteArray data; QBuffer buffer(&data); buffer.open(QIODevice::WriteOnly);
        if (image.isNull() || !image.save(&buffer, "PNG")) {
            if (error) *error = QStringLiteral("Could not encode the application icon.");
            return false;
        }
        images << data;
    }
    QByteArray bytes;
    appendU16(bytes, 0); appendU16(bytes, 1); appendU16(bytes, quint16(sizes.size()));
    quint32 offset = 6u + 16u * quint32(sizes.size());
    for (int index = 0; index < sizes.size(); ++index) {
        bytes.append(char(sizes[index] == 256 ? 0 : sizes[index]));
        bytes.append(char(sizes[index] == 256 ? 0 : sizes[index]));
        bytes.append(char(0)); bytes.append(char(0));
        appendU16(bytes, 1); appendU16(bytes, 32);
        appendU32(bytes, quint32(images[index].size())); appendU32(bytes, offset);
        offset += quint32(images[index].size());
    }
    for (const auto& image : images) bytes.append(image);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("Could not create the icon directory.");
        return false;
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        if (error) *error = output.errorString();
        return false;
    }
    return true;
}

QString createShortcutIcon(const QString& softwareExecutable, const QString& directory, QString* error) {
    if (error) error->clear();
    const QFileInfo software(softwareExecutable);
    if (!software.isFile()) return {};
    QFile artwork(QStringLiteral(":/UcalManager/ucal.png"));
    if (!artwork.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("The UCAL logo is unavailable.");
        return {};
    }
    const auto originalBytes = artwork.readAll();
    QByteArray identity = software.absoluteFilePath().toCaseFolded().toUtf8();
    identity += QByteArray::number(software.size()) + QByteArray::number(software.lastModified().toMSecsSinceEpoch());
    identity += originalBytes;
    const auto name = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    const auto path = QDir(directory).filePath(name + QStringLiteral(".ico"));
    if (QFileInfo(path).isFile()) return path;
    QFileIconProvider provider;
    const auto original = provider.icon(software);
    const auto badge = QImage::fromData(originalBytes, "PNG");
    QIcon composed;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        const auto image = composeApplicationIcon(original.pixmap(size, size).toImage(), badge, size);
        if (!image.isNull()) composed.addPixmap(QPixmap::fromImage(image));
    }
    if (!writeIconFile(path, composed, error)) return {};
    return path;
}
} // namespace Ucal::Manager
