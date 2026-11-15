#pragma once
#include <QString>
namespace Ucal::Manager {
QString resolveProductRoot(const QString& startDirectory);
QString portableDataDirectory();
// Copy only missing state; leave the source intact and never overwrite portable data.
bool migrateLegacyState(QString* error = nullptr);
}
