#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>

namespace Ucal::Manager {

// Original PE metadata only. No input code section is copied into a forwarder.
struct PeExportName { QByteArray name; quint16 slot = 0; };
struct PeExportTable {
    quint16 machine = 0;
    quint32 ordinalBase = 0;
    QVector<bool> functions;
    QVector<PeExportName> names;
};

bool readPeExports(const QByteArray& image, PeExportTable* exports, QString* error);
QByteArray makePeForwarder(const PeExportTable& exports, const QByteArray& library,
    const QMap<quint32, QByteArray>& targets, QString* error);
QByteArray makeCanoeForwarder(const QByteArray& origin, const QByteArray& data,
    quint16 machine, const QByteArray& library, const QByteArray& originModule,
    const QByteArray& dataModule, QString* error);

} // namespace Ucal::Manager
