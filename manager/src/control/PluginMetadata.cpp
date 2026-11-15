#include "PluginMetadata.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

namespace Ucal::Manager {
namespace {
QString fieldName(QString name, bool canonical) {
    if (!QRegularExpression(QStringLiteral("^[a-z][A-Za-z0-9_]*$")).match(name).hasMatch()) return name;
    if (canonical) {
        name.replace(QRegularExpression(QStringLiteral("([A-Z]+)([A-Z][a-z])")), QStringLiteral("\\1_\\2"));
        name.replace(QRegularExpression(QStringLiteral("([a-z0-9])([A-Z])")), QStringLiteral("\\1_\\2"));
        return name.toLower();
    }
    const auto words = name.split(QLatin1Char('_'));
    QString result = words.first();
    for (int index = 1; index < words.size(); ++index) {
        auto word = words[index];
        if (!word.isEmpty()) word[0] = word[0].toUpper();
        result += word;
    }
    return result;
}

bool convert(const QJsonValue& input, QJsonValue* output, bool canonical,
             QString* error, const QString& parent) {
    // These objects are dictionaries, not schema records. Integration IDs and
    // locale/environment names are opaque in both ABI representations.
    if (parent == QStringLiteral("environment") || parent == QStringLiteral("integrations")) {
        *output = input;
        return true;
    }
    if (input.isObject()) {
        QJsonObject result;
        const auto source = input.toObject();
        for (auto it = source.begin(); it != source.end(); ++it) {
            const auto key = fieldName(it.key(), canonical);
            if (result.contains(key)) {
                if (error) *error = QStringLiteral("Conflicting plugin JSON field aliases: %1").arg(key);
                return false;
            }
            QJsonValue value;
            if (!convert(it.value(), &value, canonical, error, key)) return false;
            result.insert(key, value);
        }
        *output = result;
    } else if (input.isArray()) {
        QJsonArray result;
        for (const auto& item : input.toArray()) {
            QJsonValue value;
            if (!convert(item, &value, canonical, error, parent)) return false;
            result.append(value);
        }
        *output = result;
    } else if (input.isString() && parent == QStringLiteral("purpose")) {
        *output = fieldName(input.toString(), canonical);
    } else *output = input;
    return true;
}
} // namespace

bool convertPluginJson(const QJsonValue& input, QJsonValue* output,
                         bool canonical, QString* error) {
    if (!output) return false;
    if (error) error->clear();
    return convert(input, output, canonical, error, {});
}

QString canonicalOperation(const QString& name) { return fieldName(name, true); }

bool validLocalizedMetadata(const QJsonValue& value) {
    if (value.isUndefined() || value.isString()) return true;
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!it.value().isString() || it.key().isEmpty()) return false;
    return true;
}

QString localizedMetadata(const QJsonValue& value, UiLanguage language) {
    if (value.isString()) return value.toString();
    const auto object = value.toObject();
    const auto key = uiLanguageKey(resolvedUiLanguage(language));
    const auto selected = object.value(key).toString();
    return selected.isEmpty() ? object.value(QStringLiteral("en")).toString() : selected;
}
} // namespace Ucal::Manager
