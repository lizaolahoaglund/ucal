#include "CanoeText.h"
#include "UiPresentation.h"
#include <QHash>
namespace Ucal::Manager {
namespace {
struct Row { const char* key; const char* en; const char* zh; const char* de; const char* ja; };
const Row rows[] = {
    {"launch.product.canoePreparing", "Preparing the CANoe application environment…", "正在准备 CANoe 应用运行环境…", "CANoe-Anwendungsumgebung wird vorbereitet…", "CANoe のアプリ実行環境を準備しています…"},
    {"launch.product.canoePrepareFailed", "Cannot prepare the CANoe application environment: %1. Check the selected installation and package.", "无法准备 CANoe 应用运行环境：%1。请检查所选软件安装和发布包。", "Die CANoe-Anwendungsumgebung kann nicht vorbereitet werden: %1. Installation und Paket prüfen.", "CANoe のアプリ実行環境を準備できません: %1。選択したインストールとパッケージを確認してください。"},
    {"launch.product.canoeAssetSync", "Cannot synchronize the CANoe launch copy: %1. Close CANoe using this copy and retry; check that the package is complete.", "无法同步 CANoe 启动副本：%1。请关闭正在使用此副本的 CANoe 后重试，并检查发布包是否完整。", "Die CANoe-Startkopie kann nicht synchronisiert werden: %1. CANoe mit dieser Kopie schließen und erneut versuchen; das Paket auf Vollständigkeit prüfen.", "CANoe 起動コピーを同期できません: %1。このコピーを使用中の CANoe を終了して再試行し、パッケージが完全であることを確認してください。"},
    {"editor.route.vxlProcess", "Process-level VXL route; application-specific capability inspection is required.", "进程级 VXL 路线；必须通过针对该应用的能力检查。", "VXL-Route auf Prozessebene; eine anwendungsspezifische Funktionsprüfung ist erforderlich.", "プロセス単位の VXL 経路です。アプリ固有の機能検査が必要です。"},

    {"editor.route.canoeProviderCandidate", "Candidate Provider route. Validate connection and traffic on the selected CANoe installation.", "候选 Provider 接入，需在当前 CANoe 安装上验证连接和收发。", "Provider-Kandidat. Verbindung und Datenverkehr in der gewählten CANoe-Installation prüfen.", "Provider 候補接続です。選択した CANoe で接続と送受信を確認してください。"},

    {"software.compat.vxl", "XL interface route · check compatibility before use", "XL 接口接入 · 使用前请检查兼容性", "XL-Schnittstelle · Kompatibilität vor Verwendung prüfen", "XL インターフェース · 使用前に互換性を確認"},
    {"vxl.title", "VXL compatibility deployment", "VXL 兼容层部署", "VXL-Kompatibilitätsbereitstellung", "VXL 互換レイヤー配備"},
    {"vxl.executableUnavailable", "The selected application executable is unavailable.", "所选应用的 EXE 不可用。", "Die ausgewählte Anwendungsdatei ist nicht verfügbar.", "選択したアプリの EXE を利用できません。"},
    {"vxl.helperMissing", "Deployment to the software installation folder is unavailable in this copy.", "当前副本不支持部署到软件安装文件夹。", "Bereitstellung im Softwareordner ist in dieser Kopie nicht verfügbar.", "このコピーはソフトウェアフォルダーへの配備に対応していません。"},
    {"vxl.profileUnavailable", "Deployment to the installation folder is not supported for this software.", "此软件不支持部署到安装文件夹。", "Bereitstellung im Installationsordner wird für diese Software nicht unterstützt.", "このソフトウェアはインストール先への配備に対応していません。"},
    {"vxl.heading", "Capability-gated application-local deployment", "按能力检查的应用本地部署", "Anwendungslokale Bereitstellung nach Funktionsprüfung", "機能確認付きアプリローカル配備"},
    {"vxl.explanation", "This path is only for software that statically imports the public Vector XL API. Inspect is read-only. Apply backs up adjacent API files, verifies hashes and architecture, and never changes the executable. CANoe and PCAN-View are not enabled through this path unless they pass the same public import contract.", "此路径仅用于静态导入公开 Vector XL API 的软件。检查操作只读；应用操作会备份相邻 API 文件并校验哈希和架构，绝不修改 EXE。CANoe 和 PCAN-View 只有通过同一公开导入契约时才可使用此路径。", "Dieser Pfad gilt nur für Software mit statischem Import der öffentlichen Vector-XL-API. Prüfen ist schreibgeschützt. Anwenden sichert benachbarte API-Dateien, prüft Hashes und Architektur und ändert die EXE nie. CANoe und PCAN-View werden nur bei demselben öffentlichen Importvertrag zugelassen.", "この経路は公開 Vector XL API を静的にインポートするソフトウェア専用です。検査は読み取り専用です。適用時は隣接 API ファイルをバックアップし、ハッシュとアーキテクチャを検証します。EXE は変更しません。CANoe と PCAN-View も同じ公開インポート契約を満たす場合に限ります。"},
    {"vxl.authorization", "I am authorized to modify files beside this application executable.", "我确认有权修改此应用 EXE 旁的文件。", "Ich bin berechtigt, Dateien neben dieser Anwendungsdatei zu ändern.", "このアプリ EXE に隣接するファイルを変更する権限があります。"},
    {"vxl.inspect", "Inspect compatibility", "检查兼容性", "Kompatibilität prüfen", "互換性を検査"},
    {"vxl.restore", "Restore latest deployment", "恢复最近一次部署", "Letzte Bereitstellung wiederherstellen", "最新の配備を復元"},
    {"vxl.noBackup", "No matching deployment backup is available.", "没有可用的匹配部署备份。", "Keine passende Bereitstellungssicherung ist verfügbar.", "一致する配備バックアップはありません。"},
    {"vxl.restoreQuestion", "Restore only the files recorded by the latest exact deployment manifest? Files changed since Apply will be rejected.", "只恢复最近一次精确部署清单记录的文件吗？应用后被改动的文件将被拒绝。", "Nur die im letzten exakten Bereitstellungsmanifest erfassten Dateien wiederherstellen? Seit dem Anwenden geänderte Dateien werden abgelehnt.", "最新の正確な配備マニフェストに記録されたファイルだけを復元しますか？適用後に変更されたファイルは拒否されます。"},
    {"vxl.invalidRequest", "The deployment request is invalid or stale.", "部署请求无效或已过期。", "Die Bereitstellungsanforderung ist ungültig oder veraltet.", "配備要求が無効または期限切れです。"},
    {"vxl.evidenceDirFailed", "Could not create the operation evidence directory.", "无法创建操作证据目录。", "Das Verzeichnis für Vorgangsnachweise konnte nicht erstellt werden.", "操作証拠ディレクトリを作成できませんでした。"},
    {"vxl.startFailed", "The bounded deployment helper could not start.\n\nEvidence: %1", "无法启动受限部署工具。\n\n证据：%1", "Das begrenzte Bereitstellungswerkzeug konnte nicht gestartet werden.\n\nNachweise: %1", "制限付き配備ツールを起動できませんでした。\n\n証拠：%1"},
    {"vxl.transactionFailed", "The %1 transaction failed closed.%2\n\nEvidence: %3", "%1 事务以安全关闭方式失败。%2\n\n证据：%3", "Die Transaktion %1 ist sicher geschlossen fehlgeschlagen.%2\n\nNachweise: %3", "%1 トランザクションは安全側で失敗しました。%2\n\n証拠：%3"},
    {"vxl.failedStatus", "VXL %1 failed closed", "VXL %1 已安全失败", "VXL %1 ist sicher fehlgeschlagen", "VXL %1 は安全側で失敗しました"},
    {"vxl.invalidResult", "The helper returned an invalid result contract.\n\nEvidence: %1", "部署工具返回了无效的结果契约。\n\n证据：%1", "Das Werkzeug lieferte einen ungültigen Ergebnisvertrag.\n\nNachweise: %1", "ツールが無効な結果契約を返しました。\n\n証拠：%1"},
    {"vxl.executableMutation", "The result contract reported executable mutation.", "结果契约报告 EXE 被修改。", "Der Ergebnisvertrag meldet eine Änderung der EXE.", "結果契約で EXE の変更が報告されました。"},
    {"vxl.mismatchedContract", "Inspect returned a mismatched compatibility contract.", "检查返回了不匹配的兼容性契约。", "Die Prüfung lieferte einen unpassenden Kompatibilitätsvertrag.", "検査結果の互換性契約が一致しません。"},
    {"vxl.applyQuestion", "The target has a compatible %1 static VXL import.\n\nApply will back up and transactionally replace only the adjacent public XL API and UCAL discovery files. The executable is not modified. Continue?", "目标具有兼容的 %1 静态 VXL 导入。\n\n应用操作只会备份并事务性替换相邻的公开 XL API 和 UCAL 发现文件，不会修改 EXE。继续吗？", "Das Ziel besitzt einen kompatiblen statischen %1-VXL-Import.\n\nAnwenden sichert und ersetzt ausschließlich die benachbarten öffentlichen XL-API- und UCAL-Erkennungsdateien. Die EXE wird nicht geändert. Fortfahren?", "対象には互換性のある %1 静的 VXL インポートがあります。\n\n適用時は隣接する公開 XL API と UCAL 検出ファイルだけをバックアップして置換します。EXE は変更しません。続行しますか？"},
    {"vxl.apply", "Apply VXL compatibility", "应用 VXL 兼容层", "VXL-Kompatibilität anwenden", "VXL 互換レイヤーを適用"},
    {"vxl.unexpectedStatus", "The transaction returned an unexpected status.", "事务返回了意外状态。", "Die Transaktion lieferte einen unerwarteten Status.", "トランザクションが予期しない状態を返しました。"},
    {"vxl.completedStatus", "VXL deployment %1 · %2", "VXL 部署 %1 · %2", "VXL-Bereitstellung %1 · %2", "VXL 配備 %1 · %2"},
    {"vxl.completed", "The transaction completed: %1.\n\nBackup/evidence: %2", "事务已完成：%1。\n\n备份/证据：%2", "Die Transaktion wurde abgeschlossen: %1.\n\nSicherung/Nachweise: %2", "トランザクションが完了しました：%1。\n\nバックアップ/証拠：%2"},
    {"vxl.inProgress", "VXL %1 in progress...", "VXL %1 正在进行……", "VXL %1 wird ausgeführt...", "VXL %1 を実行中…"},
    {"vxl.action.inspect", "inspection", "检查", "Prüfung", "検査"},
    {"vxl.action.apply", "apply", "应用", "Anwenden", "適用"},
    {"vxl.action.restore", "restore", "恢复", "Wiederherstellen", "復元"},
    {"vxl.applied", "applied", "已应用", "angewendet", "適用済み"},
    {"vxl.restored", "restored", "已恢复", "wiederhergestellt", "復元済み"},
    {"launch.blocker.canoeX64", "The validated CANoe route requires an x64 installation.", "已验证的 CANoe 接入路线要求 x64 安装。", "Die validierte CANoe-Route erfordert eine x64-Installation.", "検証済みの CANoe 経路には x64 版が必要です。"},
    {"launch.blocker.canoe12", "The CANoe Provider candidate route is limited to CANoe 12.", "CANoe Provider 候选路线目前仅限 CANoe 12。", "Die CANoe-Provider-Kandidatenroute ist auf CANoe 12 begrenzt.", "CANoe Provider 候補経路は CANoe 12 に限定されています。"},
    {"launch.blocker.canoeAppLocalVersion", "The application-local route is limited to CANoe 11 and 12 x64.", "应用本地兼容路线目前仅限 CANoe 11 和 12 x64。", "Die anwendungslokale Route ist auf CANoe 11 und 12 x64 begrenzt.", "アプリローカル経路は CANoe 11 および 12 x64 に限定されています。"},
    {"launch.blocker.canoeDriverRegistryMissing", "Vector driver registration is missing or empty in the 64-bit registry view:\n%1\n\nRepair the installation with the official Vector Driver Setup, then check again. If a VN1610 is connected, also check:\n%2\n\nWindows Device Manager recognizing the device does not ensure that CANoe can enumerate its channels.", "64 位注册表视图中的 Vector 驱动注册入口缺失或为空：\n%1\n\n请使用官方 Vector Driver Setup 修复安装后重新检查。若已连接 VN1610，还应检查：\n%2\n\nWindows 设备管理器识别正常，并不代表 CANoe 已能枚举其通道。", "Die Vector-Treiberregistrierung fehlt oder ist leer (64-Bit-Registrierungsansicht):\n%1\n\nReparieren Sie die Installation mit dem offiziellen Vector Driver Setup und prüfen Sie erneut. Bei angeschlossenem VN1610 prüfen Sie außerdem:\n%2\n\nDie Erkennung im Windows-Geräte-Manager garantiert keine Kanalerkennung in CANoe.", "64 ビットのレジストリ表示に Vector ドライバー登録が存在しないか、空です：\n%1\n\n公式 Vector Driver Setup でインストールを修復して再確認してください。VN1610 を接続している場合は、次の項目も確認してください：\n%2\n\nWindows デバイス マネージャーで認識されていても、CANoe がチャンネルを列挙できるとは限りません。"},
    {"launch.blocker.canoeDriverRegistryUnreadable", "Vector driver registration could not be read in the 64-bit registry view:\n%1\n\nCheck access permissions and the official Vector driver installation, then check again.", "无法读取 64 位注册表视图中的 Vector 驱动注册入口：\n%1\n\n请检查读取权限及官方 Vector 驱动安装后重新检查。", "Die Vector-Treiberregistrierung konnte nicht gelesen werden (64-Bit-Registrierungsansicht):\n%1\n\nPrüfen Sie die Zugriffsrechte und die offizielle Vector-Treiberinstallation und prüfen Sie erneut.", "64 ビットのレジストリ表示にある Vector ドライバー登録を読み取れません：\n%1\n\n読み取り権限と公式 Vector ドライバーのインストールを確認して再確認してください。"},
    {"launch.product.canoeAdapterUnsupported", "The selected CANoe integration is not supported.", "不支持所选 CANoe 接入方式。", "Die ausgewählte CANoe-Integration wird nicht unterstützt.", "選択した CANoe 統合方式はサポートされていません。"},
    {"launch.product.canoeRouteIncomplete", "The selected CANoe integration route is incomplete: %1", "所选 CANoe 接入路线不完整：%1", "Die ausgewählte CANoe-Integrationsroute ist unvollständig: %1", "選択した CANoe 統合経路が不完全です：%1"},
    {"launch.product.canoeShadowIncomplete", "The UCAL CANoe launch shadow is incomplete: %1", "UCAL CANoe 启动影子副本不完整：%1", "Der UCAL-CANoe-Start-Schatten ist unvollständig: %1", "UCAL CANoe 起動シャドウが不完全です：%1"},
    {"launch.product.peakRuntimeMissing", "The installed x64 PEAK PcanApi.dll runtime was not found.", "找不到已安装的 x64 PEAK PcanApi.dll 运行库。", "Die installierte x64-Laufzeit PEAK PcanApi.dll wurde nicht gefunden.", "インストール済みの x64 PEAK PcanApi.dll ランタイムが見つかりません。"},
    {"software.compat.canoeAccepted", "CANoe 12 x64 · available", "CANoe 12 x64 · 可创建应用", "CANoe 12 x64 · verfügbar", "CANoe 12 x64 · 利用可能"},
    {"software.compat.canoeUnsupported", "Unsupported CANoe version or architecture for the evidenced route", "此 CANoe 版本或架构不在已有证据的路线范围内", "CANoe-Version oder Architektur wird von der belegten Route nicht unterstützt", "この CANoe バージョンまたはアーキテクチャは実証済み経路の対象外です"},
    {"editor.provider", "CANoe", "CANoe", "CANoe", "CANoe"},
    {"editor.processRoute", "CANoe / Vector XL", "CANoe / Vector XL", "CANoe / Vector XL", "CANoe / Vector XL"},
    {"editor.versionAdapter", "PCAN-View 5/6", "PCAN-View 5/6", "PCAN-View 5/6", "PCAN-View 5/6"},
};
}
QString canoeProfileText(UiLanguage language, const QString& key) {
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
