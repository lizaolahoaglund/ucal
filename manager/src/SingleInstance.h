#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QLocalServer;
class QLockFile;

namespace Ucal::Manager {

class SingleInstanceController final : public QObject {
    Q_OBJECT

public:
    explicit SingleInstanceController(QObject* parent = nullptr);
    ~SingleInstanceController() override;

    bool acquirePrimary(const QString& workspacePath, QString* error = nullptr);
    static bool forwardToPrimary(const QString& workspacePath, const QString& profileId,
                                 QString* error = nullptr);
    static QString serverNameForWorkspace(const QString& workspacePath);

signals:
    void requestReceived(const QString& profileId);

private:
    std::unique_ptr<QLockFile> lock_;
    std::unique_ptr<QLocalServer> server_;
    QString serverName_;
};

}  // namespace Ucal::Manager
