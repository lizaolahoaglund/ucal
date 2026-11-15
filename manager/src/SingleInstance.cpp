#include "SingleInstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace Ucal::Manager {
namespace {

QByteArray commandPayload(const QString& profileId) {
    QJsonObject command;
    command.insert(QStringLiteral("schemaVersion"), 1);
    command.insert(QStringLiteral("command"),
                   profileId.isEmpty() ? QStringLiteral("activate")
                                       : QStringLiteral("launch-profile"));
    if (!profileId.isEmpty()) command.insert(QStringLiteral("profileId"), profileId);
    return QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
}

QString normalizedWorkspace(const QString& workspacePath) {
    QString result = QDir::cleanPath(QFileInfo(workspacePath).absoluteFilePath());
#ifdef Q_OS_WIN
    result = result.toLower();
#endif
    return result;
}

}  // namespace

SingleInstanceController::SingleInstanceController(QObject* parent) : QObject(parent) {}

SingleInstanceController::~SingleInstanceController() = default;

QString SingleInstanceController::serverNameForWorkspace(const QString& workspacePath) {
    const QByteArray digest = QCryptographicHash::hash(
        normalizedWorkspace(workspacePath).toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("UcalManager-%1").arg(QString::fromLatin1(digest.left(24)));
}

bool SingleInstanceController::acquirePrimary(const QString& workspacePath, QString* error) {
    if (error != nullptr) error->clear();
    serverName_ = serverNameForWorkspace(workspacePath);
    const QString lockPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                 .filePath(serverName_ + QStringLiteral(".lock"));
    lock_ = std::make_unique<QLockFile>(lockPath);
    lock_->setStaleLockTime(1000);
    if (!lock_->tryLock(0)) {
        lock_.reset();
        return false;
    }

    server_ = std::make_unique<QLocalServer>();
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(serverName_);
    if (!server_->listen(serverName_)) {
        if (error != nullptr) *error = server_->errorString();
        server_.reset();
        lock_->unlock();
        lock_.reset();
        return false;
    }

    connect(server_.get(), &QLocalServer::newConnection, this, [this]() {
        while (server_ != nullptr && server_->hasPendingConnections()) {
            QLocalSocket* socket = server_->nextPendingConnection();
            if (socket == nullptr) continue;
            connect(socket, &QLocalSocket::readyRead, socket, [this, socket]() {
                QByteArray buffer = socket->property("ucalCommandBuffer").toByteArray();
                buffer += socket->readAll();
                const int newline = buffer.indexOf('\n');
                if (newline < 0) {
                    if (buffer.size() > 4096) socket->disconnectFromServer();
                    else socket->setProperty("ucalCommandBuffer", buffer);
                    return;
                }
                QJsonParseError parseError{};
                const QJsonDocument document = QJsonDocument::fromJson(
                    buffer.left(newline), &parseError);
                QString profileId;
                bool valid = parseError.error == QJsonParseError::NoError && document.isObject();
                if (valid) {
                    const QJsonObject command = document.object();
                    const QString kind = command.value(QStringLiteral("command")).toString();
                    profileId = command.value(QStringLiteral("profileId")).toString().trimmed();
                    valid = command.value(QStringLiteral("schemaVersion")).toInt() == 1 &&
                        (kind == QStringLiteral("activate") ||
                         (kind == QStringLiteral("launch-profile") && !profileId.isEmpty() &&
                          profileId.size() <= 256));
                    if (kind == QStringLiteral("activate")) profileId.clear();
                }
                socket->write(valid ? QByteArrayLiteral("OK\n") : QByteArrayLiteral("ERROR\n"));
                socket->flush();
                socket->disconnectFromServer();
                // Request handlers may run a modal event loop. Finish the socket
                // callback before dispatch so disconnected/deleteLater cannot
                // invalidate a socket still in use by this callback.
                if (valid) QTimer::singleShot(0, this, [this, profileId]() {
                    emit requestReceived(profileId);
                });
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    return true;
}

bool SingleInstanceController::forwardToPrimary(const QString& workspacePath,
                                                const QString& profileId,
                                                QString* error) {
    if (error != nullptr) error->clear();
    const QString serverName = serverNameForWorkspace(workspacePath);
#ifdef Q_OS_WIN
    const QString lockPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                 .filePath(serverName + QStringLiteral(".lock"));
    QLockFile primaryLock(lockPath);
    qint64 ownerPid = 0;
    QString ownerHost, ownerApplication;
    if (primaryLock.getLockInfo(&ownerPid, &ownerHost, &ownerApplication) && ownerPid > 0)
        AllowSetForegroundWindow(static_cast<DWORD>(ownerPid));
#endif
    for (int attempt = 0; attempt < 20; ++attempt) {
        QLocalSocket socket;
        socket.connectToServer(serverName, QIODevice::ReadWrite);
        if (!socket.waitForConnected(100)) {
            QThread::msleep(50);
            continue;
        }
        const QByteArray payload = commandPayload(profileId.trimmed());
        if (socket.write(payload) != payload.size() ||
            (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(500)) ||
            (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(1000))) {
            if (error != nullptr) *error = socket.errorString();
            return false;
        }
        if (socket.readAll().startsWith("OK")) return true;
        if (error != nullptr) *error = QStringLiteral("The running UCAL Manager rejected the request.");
        return false;
    }
    if (error != nullptr) {
        *error = QStringLiteral("The running UCAL Manager did not accept the request in time.");
    }
    return false;
}

}  // namespace Ucal::Manager
