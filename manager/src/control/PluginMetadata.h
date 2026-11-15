#pragma once

#include "UiPresentation.h"
#include <QJsonValue>

namespace Ucal::Manager {

// Canonical public V2 JSON and the frozen V1 compatibility representation.
// Environment keys and integration IDs are opaque; schema aliases conflict.
bool convertPluginJson(const QJsonValue& input, QJsonValue* output,
                         bool canonical, QString* error = nullptr);
QString canonicalOperation(const QString& name);
QString localizedMetadata(const QJsonValue& value, UiLanguage language);
bool validLocalizedMetadata(const QJsonValue& value);

} // namespace Ucal::Manager
