#include "PcanViewText.h"
#include "UiPresentation.h"
#include <QHash>
namespace Ucal::Manager {
namespace {
struct Row { const char* key; const char* en; const char* zh; const char* de; const char* ja; };
const Row rows[] = {


    {"launch.blocker.pcanX64", "The PCAN-View comparator requires an x64 installation.", "PCAN-View 工程比较器要求 x64 安装。", "Der PCAN-View-Vergleichsadapter erfordert eine x64-Installation.", "PCAN-View 比較アダプターには x64 版が必要です。"},
    {"launch.blocker.pcanVersion", "The PCAN-View comparator supports only the accepted x64 version 5/6 profiles.", "PCAN-View 工程比较器仅支持已验收的 x64 版本 5/6。", "Der PCAN-View-Vergleichsadapter unterstützt nur die abgenommenen x64-Profile der Versionen 5 und 6.", "PCAN-View 比較アダプターは受け入れ済みの x64 版 5/6 のみをサポートします。"},
    {"launch.blocker.pcanOneEndpoint", "The PCAN-View comparator requires exactly one resolved CAN endpoint.", "PCAN-View 工程比较器要求恰好解析出一个 CAN 端点。", "Der PCAN-View-Vergleichsadapter benötigt genau einen aufgelösten CAN-Endpunkt.", "PCAN-View 比較アダプターでは CAN エンドポイントを 1 つに解決する必要があります。"},
    {"launch.blocker.pcanResolution", "The PCAN-View comparator did not resolve exactly one CAN endpoint.", "PCAN-View 工程比较器未能解析出恰好一个 CAN 端点。", "Für den PCAN-View-Vergleichsadapter wurde nicht genau ein CAN-Endpunkt aufgelöst.", "PCAN-View 比較アダプターで CAN エンドポイントを 1 つに解決できませんでした。"},
    {"launch.product.pcanBridgeUnavailable", "The packaged PCAN-View bridge or adapter is unavailable.", "产品包中缺少 PCAN-View bridge 或 adapter。", "Die bereitgestellte PCAN-View-Bridge oder der Adapter ist nicht verfügbar.", "製品パッケージに PCAN-View ブリッジまたはアダプターがありません。"},
    {"launch.product.pcanVerificationFailed", "The selected PCAN-View executable could not be verified.", "无法验证所选 PCAN-View EXE。", "Die ausgewählte PCAN-View-Programmdatei konnte nicht geprüft werden.", "選択した PCAN-View EXE を検証できません。"},
    {"launch.product.pcanSlotsExhausted", "All 16 PCAN-View bridge slots are in use.", "16 个 PCAN-View bridge 槽位均在使用中。", "Alle 16 PCAN-View-Bridge-Slots werden bereits verwendet.", "16 個の PCAN-View ブリッジスロットがすべて使用中です。"},
    {"software.compat.pcanComparator", "PCAN-View 5/6 x64 · available", "PCAN-View 5/6 x64 · 可创建应用", "PCAN-View 5/6 x64 · verfügbar", "PCAN-View 5/6 x64 · 利用可能"},
    {"software.compat.pcanUnsupported", "Unsupported: the existing comparator is limited to PCAN-View 5/6 x64", "不支持：现有比较器仅适用于 PCAN-View 5/6 x64", "Nicht unterstützt: Die vorhandene Vergleichsroute ist auf PCAN-View 5/6 x64 begrenzt", "未対応：既存の比較経路は PCAN-View 5/6 x64 に限定されています"},
};
}
QString pcanViewProfileText(UiLanguage language, const QString& key) {
    for (const auto& row : rows) {
        if (key != QLatin1String(row.key)) continue;
        switch (resolvedUiLanguage(language)) {
        case UiLanguage::SimplifiedChinese: return QString::fromUtf8(row.zh);
        case UiLanguage::German: return QString::fromUtf8(row.de);
        case UiLanguage::Japanese: return QString::fromUtf8(row.ja);
        default: return QString::fromUtf8(row.en);
        }
    }
    return uiText(language, key);
}
}
