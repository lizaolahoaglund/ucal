#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace Ucal::Manager {

enum class PresentationMode {
    Production,
    Explanatory,
    Realistic,
};

enum class UiLanguage {
    System,
    English,
    SimplifiedChinese,
    German,
    Japanese,
};

PresentationMode presentationModeFromArguments(const QStringList& arguments,
                                               const QString& executablePath);
QString presentationModeKey(PresentationMode mode);

UiLanguage uiLanguageFromKey(const QString& key);
QString uiLanguageKey(UiLanguage language);
UiLanguage resolvedUiLanguage(UiLanguage configuredLanguage);
QString uiLanguageOptionText(UiLanguage option, UiLanguage configuredLanguage);

UiLanguage loadUiLanguagePreference();
bool saveUiLanguagePreference(UiLanguage language);
QString uiLanguagePreferencePath();

QString uiText(UiLanguage configuredLanguage, const QString& key);
QString duplicateEndpointMessage(UiLanguage language, const QVector<int>& channels);
void installStandardButtonTranslations(UiLanguage language);

}  // namespace Ucal::Manager
