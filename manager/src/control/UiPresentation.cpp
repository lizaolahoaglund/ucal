#include "UiPresentation.h"
#include "PortablePaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <QStandardPaths>
#include <QTranslator>
#include <QHash>

namespace Ucal::Manager {
namespace {

class StandardButtonTranslator final : public QTranslator {
public:
    UiLanguage language = UiLanguage::English;
    bool isEmpty() const override { return false; }
    QString translate(const char* context, const char* source,
                      const char* = nullptr, int = -1) const override {
        const QByteArray scope(context);
        if (scope != "QPlatformTheme" && scope != "QDialogButtonBox" &&
            scope != "QMessageBox" && scope != "QInputDialog") return QString();
        QString label = QString::fromUtf8(source);
        label.remove(QLatin1Char('&'));
        const QHash<QString, QString> keys{{QStringLiteral("OK"), QStringLiteral("common.ok")},
            {QStringLiteral("Yes"), QStringLiteral("common.yes")},
            {QStringLiteral("No"), QStringLiteral("common.no")},
            {QStringLiteral("Close"), QStringLiteral("common.close")},
            {QStringLiteral("Save"), QStringLiteral("common.save")},
            {QStringLiteral("Cancel"), QStringLiteral("common.cancel")},
            {QStringLiteral("Show Details..."), QStringLiteral("editor.technicalDetails")},
            {QStringLiteral("Hide Details..."), QStringLiteral("editor.hideDetails")}};
        return keys.contains(label) ? uiText(language, keys.value(label)) : QString();
    }
};

struct TranslationRow {
    const char* key;
    const char* en;
    const char* zh;
    const char* de;
    const char* ja;
};

const TranslationRow kTranslations[] = {
    {"packages.operationUnavailable", "This operation is not provided by the software plugin.", "软件插件未提供此操作。", "Dieses Software-Plugin stellt den Vorgang nicht bereit.", "ソフトウェアプラグインはこの操作を提供していません。"},
    {"status.serviceOnline", "Service online", "服务在线", "Dienst verbunden", "サービス接続済み"},
    {"status.servicePending", "Waiting for inventory", "等待硬件清单", "Warte auf Hardwareliste", "一覧を待機中"},
    {"status.ready", "Ready", "就绪", "Ready", "Ready"},
    {"project.defaultRetained", "The default project contains all applications and is kept. Custom projects can be deleted.", "默认项目汇总所有应用，予以保留；自建项目可以删除。", "The default project contains all applications and is kept. Custom projects can be deleted.", "The default project contains all applications and is kept. Custom projects can be deleted."},
    {"project.deleteQuestion", "Delete project %1? Application configurations are kept.", "删除项目“%1”吗？其中的应用配置会保留。", "Delete project %1? Application configurations are kept.", "Delete project %1? Application configurations are kept."},
    {"editor.removeChannelQuestion", "Remove logical CAN %1? Later rows will move up after saving.", "删除逻辑通道 CAN %1 吗？保存后，后续通道序号会依次前移。", "Remove logical CAN %1? Later rows will move up after saving.", "Remove logical CAN %1? Later rows will move up after saving."},
    {"backends.library", "Driver DLL", "驱动 DLL", "Driver DLL", "Driver DLL"},
    {"backends.libraryVersion", "DLL file version", "DLL 文件版本", "DLL file version", "DLL file version"},
    {"backends.templateType", "Backend type", "后端类型", "Backend type", "Backend type"},
    {"packages.configurationProgram", "Configuration program", "配置程序", "Configuration program", "Configuration program"},
    {"plugins.noChannels", "No configured channels", "未配置通道", "Keine konfigurierten Kanäle", "構成済みチャンネルなし"},
    {"packages.configure", "Open vendor configuration", "打开厂商配置工具", "Herstellerkonfiguration öffnen", "メーカー設定を開く"},
    {"packages.restore", "Restore deployment", "恢复部署", "Bereitstellung wiederherstellen", "配置を復元"},
    {"editor.selectedOnly", "This software uses explicitly selected channels.", "此软件使用明确指定的通道。", "Diese Software verwendet ausdrücklich ausgewählte Kanäle.", "このソフトウェアではチャンネルを明示的に選択します。"},
    {"launch.configurationMissing", "The selected configuration file is missing. Select an existing file in application settings.", "选择的配置文件不存在，请在应用设置中重新选择现有文件。", "Die ausgewählte Konfigurationsdatei fehlt. Eine vorhandene Datei auswählen.", "選択した設定ファイルがありません。既存のファイルを選択してください。"},
    {"editor.configurationHelp", "Optional .cfg file to open at launch", "可选：启动时打开的 .cfg 文件", "Optional: beim Start zu öffnende .cfg-Datei", "任意：起動時に開く .cfg ファイル"},
    {"editor.configuration", "Configuration file", "配置文件", "Konfigurationsdatei", "設定ファイル"},
    {"about.sourceHelp", "This build does not include an open-source license. Use is governed by the supplied agreements. Project information is available in the accompanying README.", "本副本未附带开源许可证。使用范围以随附协议为准，项目信息见随附的 README。", "Dieser Build enthält keine Open-Source-Lizenz. Es gelten die mitgelieferten Vereinbarungen; Projektinformationen stehen in der README.", "この配布物にはオープンソースライセンスが付属していません。利用条件は付属契約を参照してください。プロジェクト情報は README にあります。"},
    {"backends.liveHelp", "Reported by the current Broker. Application compatibility is checked per profile.", "由当前 Broker 提供；应用兼容性按各应用配置单独检查。", "Vom aktuellen Broker gemeldet. Anwendungskompatibilität wird je Profil geprüft.", "現在の Broker が提供します。アプリ互換性は設定ごとに確認します。"},
    {"backends.liveSummary", "%1 endpoints · Broker connected", "%1 个端点 · Broker 已连接", "%1 Endpunkte · Broker verbunden", "%1 エンドポイント · Broker 接続済み"},
    {"backends.liveTitle", "Connected backends", "当前后端", "Verbundene backends", "接続中のバックエンド"},
    {"backends.invalidHelp", "This is not a valid backend manifest. Select a backend-registration JSON file. See details for the exact validation error.", "这不是有效的后端清单。请选择后端注册 JSON 文件；具体校验原因见“技术详情”。", "Ungültiges Backend-Manifest. Eine Backend-Registrierungsdatei auswählen; Details zeigen den Validierungsfehler.", "有効なバックエンド定義ではありません。登録用 JSON ファイルを選択してください。詳細に検証理由を表示します。"},
    {"status.reconnectHelp", "Cannot connect to UCAL. Check the runtime files, then rescan to reconnect. Saved mappings are retained.", "无法连接 UCAL。请检查运行文件，再点击“重新扫描”连接；已保存的通道配置会保留。", "Keine Verbindung mit UCAL. Laufzeitdateien prüfen und erneut suchen. Zuordnungen bleiben erhalten.", "UCAL に接続できません。実行ファイルを確認して再スキャンしてください。設定は保持されます。"},
    {"apps.lastFailure", "The last launch or run failed. Open Check compatibility for error details.", "上次启动或运行失败，请打开“检查兼容性”查看错误详情。", "Der letzte Start oder Lauf ist fehlgeschlagen. Fehlerdetails unter „Kompatibilität prüfen“.", "前回の起動または実行に失敗しました。「互換性を確認」で詳細を確認してください。"},
    {"editor.connectionInApp", "After launch, connect the channel and send or receive messages in the application.", "启动后，在软件中连接通道并收发报文。", "Nach dem Start den Kanal in der Anwendung verbinden und Nachrichten senden oder empfangen.", "起動後、ソフトウェア内でチャンネルに接続して送受信します。"},
    {"editor.hideDetails", "Hide details", "收起详情", "Details ausblenden", "詳細を隠す"},
    {"editor.technicalDetails", "Technical details", "技术详情", "Technische Details", "技術詳細"},
    {"metadata.notReported", "Not provided", "未提供", "Nicht angegeben", "未提供"},
    {"common.close", "Close", "关闭", "Schließen", "閉じる"},
    {"common.no", "No", "否", "Nein", "いいえ"},
    {"common.yes", "Yes", "是", "Ja", "はい"},
    {"common.ok", "OK", "确定", "OK", "OK"},
    {"common.browse", "Browse...", "浏览…", "Durchsuchen...", "Browse..."},
    {"window.explanatory", "UCAL Manager · Interaction POC", "UCAL Manager · 交互 POC", "UCAL Manager · Interaktions-POC", "UCAL Manager · 操作 POC"},
    {"window.realistic", "UCAL Manager", "UCAL Manager", "UCAL Manager", "UCAL Manager"},
    {"project.current", "Current project", "当前项目", "Aktuelles Projekt", "現在のプロジェクト"},
    {"project.manage", "Manage", "管理", "Verwalten", "管理"},
    {"project.default", "Default project", "默认项目", "Standardprojekt", "デフォルトプロジェクト"},
    {"project.powertrain", "Powertrain calibration", "动力标定", "Antriebsstrang-Kalibrierung", "パワートレイン適合"},
    {"project.gateway", "Gateway validation", "网关验证", "Gateway-Validierung", "ゲートウェイ検証"},
    {"project.allApps", "All applications", "全部应用", "Alle Anwendungen", "すべてのアプリ"},
    {"project.appCount", "%1 applications", "%1 个应用", "%1 Anwendungen", "%1 個のアプリ"},
    {"editor.softwareName", "Software", "软件名称", "Software", "ソフトウェア"},
    {"editor.installationPath", "Installation folder", "安装路径", "Installationspfad", "インストール先"},
    {"editor.arguments", "Extra launch arguments", "更多启动参数", "Weitere Startargumente", "追加の起動引数"},
    {"editor.argumentsHelp", "Optional; quote paths that contain spaces", "可选；包含空格的路径请用双引号括起", "Optional; Pfade mit Leerzeichen in Anführungszeichen setzen", "任意。空白を含むパスは引用符で囲んでください"},
    {"software.chooseToCreate", "Choose software, then click Create application.", "选择要使用的软件，点击“创建应用”。", "Software auswählen und Anwendung erstellen anklicken.", "ソフトウェアを選び、アプリ作成をクリックしてください。"},
    {"settings.checkUpdate", "Check for updates", "检查更新", "Nach Updates suchen", "更新を確認"},
    {"settings.updateSourceMissing", "No GitHub Release repository configured yet.", "尚未配置 GitHub Release 更新源。", "Noch kein GitHub-Release-Repository konfiguriert.", "GitHub Release の更新元は未設定です。"},
    {"backends.folderSummary", "%1 driver packages", "%1 个驱动包", "%1 Treiberpakete", "%1 件のドライバーパッケージ"},
    {"backends.available", "Available", "可用", "Verfügbar", "利用可能"},
    {"backends.providerCustom", "Custom driver", "自定义驱动", "Benutzerdefinierter Treiber", "カスタムドライバー"},
    {"backends.saveProvider", "Save driver settings", "保存驱动设置", "Treibereinstellungen speichern", "ドライバー設定を保存"},
    {"backends.capabilityMismatch", "Channel capabilities do not meet the interface requirements", "通道功能不符合接入要求", "Kanalfunktionen erfüllen die Schnittstellenanforderungen nicht", "チャンネル機能がインターフェイス要件を満たしていません"},
    {"backends.runtimeIncompatible", "Incompatible runtime interface", "运行库接口不兼容", "Inkompatible Laufzeitschnittstelle", "互換性のないランタイムインターフェイス"},
    {"backends.notChecked", "Not checked", "待检查", "Nicht geprüft", "未確認"},
    {"backends.runtimeMissing", "Check vendor driver", "需检查厂商驱动", "Herstellertreiber prüfen", "ベンダードライバーを確認"},
    {"backends.notActive", "Service restart required", "需重启 UCAL 服务", "UCAL-Dienst neu starten", "UCAL サービスの再起動が必要"},
    {"backends.hardwareCount", "%1 connected CAN channels", "已连接 %1 个 CAN 通道", "%1 CAN-Kanäle verbunden", "CAN %1 チャンネル接続中"},
    {"backends.vendor", "Vendor", "厂商", "Hersteller", "ベンダー"},
    {"backends.version", "Package version", "驱动包版本", "Paketversion", "パッケージのバージョン"},
    {"backends.runtimeAutomatic", "Automatic installed x64 runtime", "自动选择已安装的 x64 运行库", "Installierte x64-Laufzeit automatisch wählen", "インストール済み x64 ランタイムを自動選択"},
    {"backends.runtimeSelectedVersion", "Selected DLL version", "选定 DLL 版本", "Version der gewählten DLL", "選択した DLL のバージョン"},
    {"backends.loadedRuntime", "Loaded runtime", "已加载运行库", "Geladene Laufzeit", "読み込まれたランタイム"},
    {"backends.runtimeRestart", "Runtime changes take effect when the service next starts.", "运行库设置在服务下次启动时生效。", "Änderungen gelten beim nächsten Dienststart.", "変更は次回のサービス起動時に適用されます。"},
    {"backends.runtimeInvalid", "Choose a valid x64 DLL.", "请选择有效的 x64 DLL。", "Wählen Sie eine gültige x64-DLL.", "有効な x64 DLL を選択してください。"},
    {"backends.provider", "J2534 provider", "J2534 驱动", "J2534-Anbieter", "J2534 プロバイダー"},
    {"hardware.canFdRequired", "ISO CAN FD required", "需要 ISO CAN FD", "ISO CAN FD erforderlich", "ISO CAN FD が必要です"},
    {"backends.providerAutomatic", "Discover installed providers automatically", "自动发现已安装的驱动", "Installierte Anbieter automatisch finden", "インストール済みプロバイダーを自動検出"},
    {"backends.openSelector", "Channel selector (optional)", "通道选择符（可选）", "Kanalauswahl (optional)", "チャンネル指定（任意）"},
    {"backends.defaultSelector", "Empty: use the driver-configured default channel", "留空使用驱动配置的默认通道", "Leer: Standardkanal des Treibers", "空欄: ドライバー設定の既定チャンネル"},
    {"backends.discoveryProgram", "Channel discovery", "通道发现程序", "Kanalsuche", "チャンネル検出プログラム"},
    {"backends.providerInvalid", "Choose a valid x86 or x64 J2534 DLL.", "请选择有效的 x86 或 x64 J2534 DLL。", "Wählen Sie eine gültige x86- oder x64-J2534-DLL.", "有効な x86 または x64 J2534 DLL を選択してください。"},
    {"backends.j2534DiscoveryHelp", "04.04 + GMW17753. The default entry uses the driver configuration; enter a documented selector only when needed. Runtime changes apply when the provider is idle.", "要求 04.04 + GMW17753。默认入口使用驱动配置；需要时填写该驱动公开的通道选择符。运行库变更在驱动空闲后生效。", "04.04 + GMW17753. Der Standardeintrag verwendet die Treiberkonfiguration. Bei Bedarf einen dokumentierten Selektor eingeben. Laufzeitänderungen gelten bei inaktivem Anbieter.", "04.04 + GMW17753 が必要です。既定の接続先にはドライバー設定を使用します。必要な場合のみ公開された選択文字列を入力します。変更はプロバイダーがアイドル状態になると適用されます。"},
    {"backends.status", "Status", "状态", "Status", "状態"},
    {"backends.capabilities", "Functions", "功能", "Funktionen", "機能"},
    {"backends.folder", "Package folder", "驱动包位置", "Paketordner", "パッケージフォルダー"},
    {"apps.empty.title", "Set up your first application", "开始使用 UCAL", "Erste Anwendung einrichten", "最初のアプリを設定"},
    {"apps.empty.help", "Three steps to connect your software to hardware. Applications are created by you.", "先准备硬件和软件，再创建自己的应用。", "In drei Schritten Software und Hardware verbinden.", "3 ステップでソフトウェアとハードウェアを接続します。"},
    {"apps.empty.hardware", "Discover hardware", "发现硬件", "Hardware finden", "ハードウェアを検出"},
    {"apps.empty.hardwareHelp", "Connect an adapter and check its available channels.", "连接设备，在“硬件”页确认可用通道。", "Adapter verbinden und verfügbare Kanäle prüfen.", "デバイスを接続し、利用可能なチャンネルを確認します。"},
    {"apps.empty.software", "Discover software", "发现软件", "Software finden", "ソフトウェアを検出"},
    {"apps.empty.softwareHelp", "Find an installation or add its executable.", "在“软件”页发现安装实例，或手动添加 EXE。", "Installation suchen oder EXE hinzufügen.", "インストールを検出するか EXE を追加します。"},
    {"apps.empty.create", "Create an application", "创建应用", "Anwendung erstellen", "アプリを作成"},
    {"apps.empty.createHelp", "Choose software, assign channels, then launch.", "选择软件、分配通道，保存后即可启动。", "Software wählen, Kanäle zuweisen und starten.", "ソフトウェアとチャンネルを選択して起動します。"},
    {"apps.empty.project", "No applications in this project yet", "当前项目还没有应用", "Noch keine Anwendungen in diesem Projekt", "このプロジェクトにはまだアプリがありません"},
    {"software.applicationCount", "%1 applications created", "已创建 %1 个应用", "%1 Anwendungen erstellt", "%1 個のアプリを作成済み"},
    {"software.applicationHelp", "Applications created from this software installation.", "以下应用使用此软件安装实例。", "Anwendungen dieser Softwareinstallation.", "このインストールから作成されたアプリです。"},
    {"backends.packageMissing", "Package unavailable", "驱动包文件缺失", "Paket nicht verfügbar", "パッケージが見つかりません"},
    {"backends.missingHelp", "The package is missing or invalid. Its last known details are retained; running connections are unchanged.", "驱动包已移除或无法读取。保留上次的信息，运行中的连接不受影响。", "Das Paket fehlt oder ist ungültig. Letzte Angaben und laufende Verbindungen bleiben erhalten.", "パッケージが見つからないか無効です。前回の情報と実行中の接続は維持されます。"},
    {"backends.scanComplete", "Driver folder refreshed · %1 packages retained", "驱动目录已刷新 · 保留 %1 个驱动包", "Treiberordner aktualisiert · %1 Pakete", "ドライバーフォルダー更新 · %1 件"},
    {"editor.addChannel", "+ Add a channel", "＋ 添加通道", "+ Kanal hinzufügen", "＋ チャンネルを追加"},
    {"mapping.duplicateEndpoint", "Duplicate hardware bindings: %1. Select a different hardware endpoint for each application channel.", "以下应用通道存在重复绑定：%1。请为每个应用通道选择不同的硬件端点。", "Doppelte Hardwarezuordnungen: %1. Wählen Sie für jeden Anwendungskanal einen anderen Hardware-Endpunkt.", "ハードウェアの割り当てが重複しているアプリケーションチャンネル：%1。各チャンネルに異なるハードウェアエンドポイントを選択してください。"},
    {"mapping.channelListSeparator", ", ", "、", ", ", "、"},
    {"mapping.endpointAssignedSuffix", " (Assigned)", "（已分配）", " (Zugewiesen)", "（割り当て済み）"},
    {"editor.channelLimit", "Up to %1 channels for this software", "此软件最多支持 %1 个通道", "Diese Software unterstützt bis zu %1 Kanäle", "このソフトウェアは最大 %1 チャンネルに対応"},
    {"hardware.channelLimit", "All %1 channel slots are assigned. Change the binding in application settings.", "%1 个通道已全部分配；请在应用设置中更换绑定。", "Alle %1 Kanäle sind belegt. Zuordnung in den Anwendungseinstellungen ändern.", "%1 チャンネルが割り当て済みです。アプリ設定で割り当てを変更してください。"},
    {"editor.created", "Created", "创建时间", "Erstellt", "作成日時"},
    {"editor.createdUnknown", "Not recorded", "未记录", "Nicht erfasst", "記録なし"},
    {"nav.applications", "Applications", "应用", "Anwendungen", "アプリ"},
    {"nav.hardware", "Hardware", "硬件", "Hardware", "ハードウェア"},
    {"nav.software", "Software", "软件", "Software", "ソフトウェア"},
    {"nav.backends", "Drivers", "驱动", "Treiber", "ドライバー"},
    {"nav.settings", "Settings", "设置", "Einstellungen", "設定"},
    {"sidebar.demoBadge", "INTERACTION POC · DEMO DATA", "交互 POC · 演示数据", "INTERAKTIONS-POC · DEMODATEN", "操作 POC · デモデータ"},
    {"sidebar.demoNote", "Does not launch vendor software, access hardware, or modify DLLs and registry settings.", "不启动厂商软件，不访问真实硬件，不修改 DLL 或注册表。", "Startet keine Herstellersoftware, greift nicht auf Hardware zu und ändert keine DLLs oder Registrierung.", "ベンダーソフトや実機を起動せず、DLL・レジストリも変更しません。"},
    {"sidebar.realBadge", "SYSTEM READY", "系统就绪", "SYSTEM BEREIT", "システム準備完了"},
    {"sidebar.realNote", "5 channels online · 3 backends loaded", "5 个通道在线 · 3 个后端已加载", "5 Kanäle online · 3 backends geladen", "5 チャンネルオンライン · 3 バックエンド読込済み"},
    {"sidebar.productionBadge", "LIVE BROKER MODE", "实时 Broker 模式", "LIVE-BROKER-MODUS", "ライブ Broker モード"},
    {"sidebar.productionNote", "Refresh reads the UCAL · mappings persist by endpoint ID", "刷新时读取 UCAL · 映射按端点 ID 持久保存", "Aktualisieren liest den UCAL · Zuordnungen bleiben nach Endpunkt-ID erhalten", "更新時に UCAL を読込み · マッピングはエンドポイント ID で保持"},
    {"common.settings", "Settings", "设置", "Einstellungen", "設定"},
    {"common.launch", "Launch", "启动", "Starten", "起動"},
    {"common.preflight", "Preflight", "预检", "Vorprüfung", "事前確認"},
    {"common.ready", "Ready", "已就绪", "Bereit", "準備完了"},
    {"common.configured", "Configured", "已配置", "Konfiguriert", "設定済み"},
    {"common.needsConfiguration", "Needs configuration", "需要配置", "Konfiguration erforderlich", "設定が必要"},
    {"common.running", "Running", "运行中", "Wird ausgeführt", "実行中"},
    {"common.attention", "Check required", "需要检查", "Prüfung erforderlich", "確認が必要"},
    {"common.online", "Online", "在线", "Online", "オンライン"},
    {"common.offline", "Offline", "离线", "Offline", "オフライン"},
    {"common.busy", "In use", "使用中", "Belegt", "使用中"},
    {"common.discovered", "Discovered", "已发现", "Gefunden", "検出済み"},
    {"common.loaded", "Loaded", "已加载", "Geladen", "読込済み"},
    {"common.disabled", "Disabled", "已禁用", "Deaktiviert", "無効"},
    {"source.registry", "Windows Registry", "Windows 注册表", "Windows-Registrierung", "Windows レジストリ"},
    {"source.appPaths", "Windows App Paths", "Windows 应用路径", "Windows App Paths", "Windows App Paths"},
    {"source.manual", "Manual", "手动添加", "Manuell", "手動追加"},
    {"source.builtin", "Built-in", "内置", "Integriert", "内蔵"},
    {"source.thirdParty", "Third-party package", "第三方包", "Drittanbieterpaket", "サードパーティパッケージ"},
    {"source.ready", "Ready", "已就绪", "Bereit", "準備完了"},
    {"common.details", "Details", "详情", "Details", "詳細"},
    {"common.revalidate", "Revalidate", "重新校验", "Erneut prüfen", "再検証"},
    {"common.cancel", "Cancel", "取消", "Abbrechen", "キャンセル"},
    {"common.open", "Open UCAL Manager", "打开 UCAL Manager", "UCAL Manager öffnen", "UCAL Manager を開く"},
    {"common.exit", "Exit", "退出", "Beenden", "終了"},
    {"config.loadFailed", "Could not load the saved configuration.\n\n%1", "无法加载已保存的配置。\n\n%1", "Die gespeicherte Konfiguration konnte nicht geladen werden.\n\n%1", "保存済みの設定を読み込めませんでした。\n\n%1"},
    {"config.saveFailed", "Could not save the configuration.\n\n%1", "无法保存配置。\n\n%1", "Die Konfiguration konnte nicht gespeichert werden.\n\n%1", "設定を保存できませんでした。\n\n%1"},
    {"config.saved", "Configuration saved", "配置已保存", "Konfiguration gespeichert", "設定を保存しました"},
    {"settings.saveFailed", "Could not save the application settings.", "无法保存应用设置。", "Die Anwendungseinstellungen konnten nicht gespeichert werden.", "アプリ設定を保存できませんでした。"},
    {"apps.eyebrow", "", "", "", ""},
    {"apps.subtitle.explain", "An application is a launchable profile; one software installation can have multiple profiles.", "应用是可启动的配置；同一份软件可以创建多套独立应用。", "Eine Anwendung ist ein startbares Profil; eine Installation kann mehrere Profile besitzen.", "アプリは起動可能なプロファイルです。1つのインストールから複数作成できます。"},
    {"apps.subtitle.real", "2 applications ready · 5 hardware channels online", "2 个应用已就绪 · 5 个硬件通道在线", "2 Anwendungen bereit · 5 Hardwarekanäle online", "2 アプリ準備完了 · 5 ハードウェアチャンネルオンライン"},
    {"apps.scan", "Rescan", "重新扫描", "Neu suchen", "再スキャン"},
    {"apps.add", "+ Add application", "+ 添加应用", "+ Anwendung hinzufügen", "+ アプリを追加"},
    {"apps.startAll", "Launch all project applications", "启动项目中的全部应用", "Alle Projektanwendungen starten", "プロジェクト内をすべて起動"},
    {"apps.startAllEmpty", "The current project has no application profiles.", "当前项目没有应用配置。", "Das aktuelle Projekt enthält keine Anwendungsprofile.", "現在のプロジェクトにアプリプロファイルはありません。"},
    {"apps.startAllQueued", "Launching %1 project application profiles.", "正在启动项目中的 %1 个应用配置。", "%1 Anwendungsprofile des Projekts werden gestartet.", "プロジェクト内の %1 個のアプリプロファイルを起動します。"},
    {"apps.subtitle.production", "%1 application profiles", "%1 个应用配置", "%1 Anwendungsprofile", "%1 アプリ構成"},
    {"apps.addToProject", "＋  Create a new software application in this project", "＋  在当前项目中创建新的软件应用", "＋  Neue Softwareanwendung im Projekt erstellen", "＋  現在のプロジェクトに新規アプリを作成"},
    {"apps.fullSummary", "All compatible channels · %1 online", "全部兼容通道 · %1 个在线", "Alle kompatiblen Kanäle · %1 online", "対応する全チャンネル · %1 オンライン"},
    {"apps.selectedSummary", "%1 mapped channels · %2 online", "映射 %1 个通道 · %2 个在线", "%1 zugeordnete Kanäle · %2 online", "%1 マッピング · %2 オンライン"},
    {"apps.allChannels", "All channels", "全部通道", "Alle Kanäle", "全チャンネル"},
    {"apps.channelCount", "%1 channels", "%1 个通道", "%1 Kanäle", "%1 チャンネル"},
    {"apps.menu.plan", "View launch summary", "查看本次启动摘要", "Startübersicht anzeigen", "起動概要を表示"},
    {"apps.menu.native", "Launch in native environment", "使用原生环境启动", "In nativer Umgebung starten", "ネイティブ環境で起動"},
    {"apps.menu.last", "Show application window", "显示应用窗口", "Anwendungsfenster anzeigen", "アプリのウィンドウを表示"},
    {"apps.stopHelp", "Close this running application; keep its saved settings.", "结束当前运行的应用，保留已保存的设置。", "Laufende Anwendung schließen; Einstellungen bleiben erhalten.", "実行中のアプリを閉じ、保存済み設定は保持します。"},
    {"apps.menu.stop", "Stop running application", "停止运行", "Laufende Anwendung beenden", "実行を停止"},
    {"apps.menu.shortcut", "Create desktop shortcut", "创建桌面快捷方式", "Desktop-Verknüpfung erstellen", "デスクトップショートカットを作成"},
    {"apps.menu.duplicate", "Duplicate application", "复制应用", "Anwendung duplizieren", "アプリを複製"},
    {"apps.menu.check", "Check compatibility", "检查兼容性", "Kompatibilität prüfen", "互換性を確認"},
    {"apps.menu.location", "Open software location", "打开软件位置", "Softwareordner öffnen", "ソフトウェアの場所を開く"},
    {"apps.menu.deploy", "Deploy UCAL to the software installation folder…", "将 UCAL 部署到软件安装文件夹…", "UCAL im Softwareordner bereitstellen…", "ソフトウェアフォルダーに UCAL を配備…"},
    {"apps.menu.delete", "Delete application", "删除应用", "Anwendung löschen", "アプリを削除"},
    {"apps.shortcutCreated", "Desktop shortcut created", "桌面快捷方式已创建", "Desktop-Verknüpfung erstellt", "デスクトップショートカットを作成しました"},
    {"apps.launchTitle", "Launch profile", "启动配置", "Profil starten", "プロファイルを起動"},
    {"apps.nativeTitle", "Native launch", "原生启动", "Nativer Start", "ネイティブ起動"},
    {"apps.launchDetails", "%1\n%2\n\nIntegration: %3\nChannels: %4\n\nStatus: %5", "%1\n%2\n\n接入：%3\n通道：%4\n\n状态：%5", "%1\n%2\n\nIntegration: %3\nKanäle: %4\n\nStatus: %5", "%1\n%2\n\n統合：%3\nチャンネル：%4\n\n状態：%5"},
    {"apps.notRunning", "This profile is not running.", "此应用配置未在运行。", "Dieses Profil wird nicht ausgeführt.", "このプロファイルは実行されていません。"},
    {"apps.runningPid", "%1 is running.\n\nProcess ID: %2", "%1 正在运行。\n\n进程 ID：%2", "%1 wird ausgeführt.\n\nProzess-ID: %2", "%1 は実行中です。\n\nプロセス ID：%2"},
    {"apps.reviewNoProcesses", "Review presentations do not own external processes.", "评审演示不会接管外部进程。", "Prüfansichten verwalten keine externen Prozesse.", "レビュー表示は外部プロセスを管理しません。"},
    {"apps.stopping", "%1 is stopping...", "正在停止 %1……", "%1 wird beendet...", "%1 を停止しています…"},
    {"apps.alreadyRunning", "%1 is already running.", "%1 已在运行。", "%1 wird bereits ausgeführt.", "%1 はすでに実行中です。"},
    {"apps.tempDataFailed", "Could not create temporary launch data.", "无法创建临时启动数据。", "Temporäre Startdaten konnten nicht erstellt werden.", "一時起動データを作成できませんでした。"},
    {"apps.launchCheck", "%1 · launch check %2", "%1 · 启动检查%2", "%1 · Startprüfung %2", "%1 · 起動確認 %2"},
    {"apps.checkReady", "passed", "通过", "bestanden", "合格"},
    {"apps.checkBlocked", "blocked", "未通过", "blockiert", "不合格"},
    {"launch.summary.application", "Application: %1", "应用：%1", "Anwendung: %1", "アプリ：%1"},
    {"launch.summary.frontend", "Frontend: %1", "前端：%1", "Frontend: %1", "フロントエンド：%1"},
    {"launch.summary.software", "Software: %1 %2 (%3)", "软件：%1 %2（%3）", "Software: %1 %2 (%3)", "ソフトウェア：%1 %2（%3）"},
    {"launch.summary.adapter", "Integration: %1", "接入方式：%1", "Integration: %1", "統合方式：%1"},
    {"launch.summary.assignment", "Assignment: %1", "通道分配：%1", "Zuordnung: %1", "チャンネル割り当て：%1"},
    {"launch.summary.channelCount", "Resolved CAN channels: %1", "已解析 CAN 通道：%1", "Aufgelöste CAN-Kanäle: %1", "解決済み CAN チャンネル：%1"},
    {"launch.summary.status", "Status: %1", "状态：%1", "Status: %1", "状態：%1"},
    {"launch.summary.ready", "ready", "可以启动", "bereit", "起動可能"},
    {"launch.summary.blocked", "blocked", "无法启动", "blockiert", "起動不可"},
    {"launch.summary.channel", "  CAN %1 -> %2 / physical port %3", "  CAN %1 → %2 / 物理端口 %3", "  CAN %1 → %2 / physischer Port %3", "  CAN %1 → %2 / 物理ポート %3"},
    {"launch.summary.blockers", "Cannot launch:", "无法启动的原因：", "Start nicht möglich:", "起動できない理由："},
    {"launch.blocker.identityIncomplete", "Logical channel %1 lacks complete stable endpoint identity.", "逻辑通道 %1 缺少完整、稳定的端点标识。", "Dem logischen Kanal %1 fehlt eine vollständige stabile Endpunktidentität.", "論理チャンネル %1 に完全で安定したエンドポイント識別情報がありません。"},
    {"launch.blocker.endpointOffline", "Logical channel %1 endpoint is not online.", "逻辑通道 %1 对应的端点不在线。", "Der Endpunkt des logischen Kanals %1 ist nicht online.", "論理チャンネル %1 のエンドポイントがオンラインではありません。"},
    {"launch.blocker.notCan", "Logical channel %1 is not a CAN endpoint.", "逻辑通道 %1 对应的端点不是 CAN 端点。", "Der Endpunkt des logischen Kanals %1 ist kein CAN-Endpunkt.", "論理チャンネル %1 のエンドポイントは CAN ではありません。"},
    {"launch.blocker.rawIndexMissing", "Logical channel %1 lacks a current physical port index; refresh the Broker inventory.", "逻辑通道 %1 缺少当前物理端口编号；请刷新 硬件清单。", "Dem logischen Kanal %1 fehlt ein aktueller physischer Portindex; aktualisieren Sie den Broker-Bestand.", "論理チャンネル %1 に現在の物理ポート番号がありません。Broker 一覧を更新してください。"},
    {"launch.blocker.profileNotFound", "Application profile was not found.", "找不到应用配置。", "Das Anwendungsprofil wurde nicht gefunden.", "アプリプロファイルが見つかりません。"},
    {"launch.blocker.softwareNotFound", "Software installation was not found.", "找不到对应的软件安装。", "Die Softwareinstallation wurde nicht gefunden.", "ソフトウェアのインストールが見つかりません。"},
    {"launch.blocker.executableUnavailable", "The selected software executable is unavailable.", "所选软件的 EXE 不可用。", "Die ausgewählte Programmdatei ist nicht verfügbar.", "選択したソフトウェアの EXE を利用できません。"},
    {"launch.blocker.inventoryStale", "Broker inventory is not current.", "硬件清单尚未刷新。", "Der Broker-Bestand ist nicht aktuell.", "Broker 一覧が最新ではありません。"},
    {"launch.blocker.routeUnsupported", "The selected software and integration do not form an accepted launch route.", "所选软件与接入方式不能组成已验收的启动路线。", "Die ausgewählte Software und Integration bilden keine abgenommene Startroute.", "選択したソフトウェアと統合方式は受け入れ済みの起動経路になりません。"},
    {"launch.blocker.selectedCount", "Selected assignment must contain 1..16 logical CAN channels.", "指定通道模式必须包含 1～16 个逻辑 CAN 通道。", "Die ausgewählte Zuordnung muss 1 bis 16 logische CAN-Kanäle enthalten.", "指定割り当てには 1～16 個の論理 CAN チャンネルが必要です。"},
    {"launch.blocker.channelUnmapped", "Logical channel %1 is not mapped.", "逻辑通道 %1 尚未映射。", "Der logische Kanal %1 ist nicht zugeordnet.", "論理チャンネル %1 は割り当てられていません。"},
    {"launch.blocker.channelUnknown", "Logical channel %1 references an unknown endpoint.", "逻辑通道 %1 引用了未知端点。", "Der logische Kanal %1 verweist auf einen unbekannten Endpunkt.", "論理チャンネル %1 が不明なエンドポイントを参照しています。"},
    {"launch.blocker.noOnlineEndpoint", "No online compatible CAN endpoint is available.", "没有在线且兼容的 CAN 端点。", "Es ist kein kompatibler CAN-Endpunkt online.", "オンラインで互換性のある CAN エンドポイントがありません。"},
    {"launch.blocker.tooManyChannels", "Full Access resolves more than the accepted 16-channel target.", "全部兼容通道解析结果超过当前验收的 16 通道目标。", "Vollzugriff ergibt mehr als die abgenommenen 16 Kanäle.", "全互換チャンネルの解決結果が受け入れ済みの 16 チャンネルを超えています。"},
    {"launch.product.routeUnavailable", "The selected application has no UCAL product launch route.", "所选应用没有可用的 UCAL 产品启动路线。", "Für die ausgewählte Anwendung ist keine UCAL-Produktstartroute verfügbar.", "選択したアプリには UCAL 製品起動経路がありません。"},
    {"apps.startFailed", "Could not start %1.\n\n%2", "无法启动 %1。\n\n%2", "%1 konnte nicht gestartet werden.\n\n%2", "%1 を起動できませんでした。\n\n%2"},
    {"apps.started", "%1 started · PID %2", "%1 已启动 · PID %2", "%1 gestartet · PID %2", "%1 を起動しました · PID %2"},
    {"apps.stopped", "%1 stopped · exit %2%3", "%1 已停止 · 退出码 %2%3", "%1 beendet · Exit-Code %2%3", "%1 は停止しました · 終了コード %2%3"},
    {"apps.crashed", " (crashed)", "（异常退出）", " (abgestürzt)", "（異常終了）"},
    {"apps.starting", "Starting %1...", "正在启动 %1……", "%1 wird gestartet...", "%1 を起動しています…"},
    {"apps.refreshBeforeLaunch", "Scanning hardware before launch…", "启动前正在扫描硬件……", "Hardware wird vor dem Start gesucht…", "起動前にハードウェアを確認中…"},
    {"apps.deleteRunning", "Stop this profile before deleting it.", "请先停止此应用配置再删除。", "Beenden Sie dieses Profil vor dem Löschen.", "削除する前にこのプロファイルを停止してください。"},
    {"hardware.eyebrow", "CHANNEL INVENTORY", "通道清单", "KANALBESTAND", "チャンネル一覧"},
    {"hardware.subtitle.explain", "Grouped by physical adapter; each channel is assigned independently.", "按物理设备分组，以通道为最小分配单位；“可见”不等于“占用”。", "Nach Adapter gruppiert; jeder Kanal wird einzeln zugewiesen.", "物理アダプターごとに表示し、チャンネル単位で割り当てます。"},
    {"hardware.subtitle.real", "3 adapters · 7 channels · 5 online", "3 个适配器 · 7 个通道 · 5 个在线", "3 Adapter · 7 Kanäle · 5 online", "3 アダプター · 7 チャンネル · 5 オンライン"},
    {"hardware.filterPlaceholder", "Driver, device, serial or channel", "按驱动、设备、序列号或通道过滤", "Treiber, Gerät, Seriennummer oder Kanal", "ドライバー・機器・シリアル・チャンネル"},
    {"hardware.filterName", "Filter hardware channels", "过滤硬件通道", "Hardwarekanäle filtern", "ハードウェアチャンネルを絞り込む"},
    {"hardware.filterEmpty", "No hardware channels match these keywords.", "没有匹配这些关键字的硬件通道。", "Keine Hardwarekanäle entsprechen diesen Suchbegriffen.", "キーワードに一致するチャンネルはありません。"},
    {"backends.enabledState", "Enabled", "已启用", "Aktiviert", "有効"},
    {"backends.enable", "Enable", "启用", "Aktivieren", "有効にする"},
    {"backends.disable", "Disable", "禁用", "Deaktivieren", "無効にする"},
    {"backends.disabledEndpoint", "Driver disabled", "驱动已禁用", "Treiber deaktiviert", "ドライバー無効"},
    {"backends.toggleHelp", "Saved for this driver package. Disabling prevents discovery and new connections; existing connections continue until closed.", "按驱动包保存启用状态。禁用后停止发现和新连接；已有连接保留到关闭。", "Gilt für dieses Treiberpaket. Deaktivieren verhindert Erkennung und neue Verbindungen; bestehende bleiben bis zum Schließen erhalten.", "ドライバー単位で保存します。無効時は検出と新規接続を停止し、既存接続は閉じるまで維持します。"},
    {"backends.saveFailed", "Could not save the driver enable state. Check that the portable data folder is writable.", "无法保存驱动启用状态，请检查程序旁 data 文件夹是否可写。", "Treiberstatus konnte nicht gespeichert werden. Prüfen Sie die Schreibrechte für data.", "ドライバー状態を保存できません。data フォルダーの書き込み権限を確認してください。"},
    {"hardware.rescan", "Rescan", "重新扫描", "Neu suchen", "再スキャン"},
    {"hardware.summary", "SN %1 · Firmware %2 · %3 channels", "SN %1 · 固件 %2 · %3 个通道", "SN %1 · Firmware %2 · %3 Kanäle", "SN %1 · FW %2 · %3 チャンネル"},
    {"hardware.bindingHelp", "Channel bindings are edited in application settings. Select an application to open its channel settings.", "通道绑定统一在应用设置中修改。选择一个应用，可打开它的通道设置。", "Kanalzuordnungen werden in den Anwendungseinstellungen bearbeitet. Wählen Sie eine Anwendung aus, um ihre Kanaleinstellungen zu öffnen.", "チャンネルの割り当てはアプリの設定で変更します。アプリを選択してチャンネル設定を開いてください。"},
    {"hardware.savedBinding", "Not found · retained binding", "未发现 · 保留的绑定", "Not found · retained binding", "Not found · retained binding"},
    {"hardware.visibleApps", "Bound to %1 applications", "已绑定 %1 个应用", "%1 Anwendungen zugeordnet", "%1 アプリに割り当て済み"},
    {"hardware.productionCached", "%1 saved CAN channels · inventory not current", "%1 个已保存 CAN 通道 · 硬件清单尚未刷新", "%1 gespeicherte CAN-Kanäle · Liste noch nicht aktualisiert", "保存済み CAN チャンネル %1 · 一覧は未更新"},
    {"hardware.productionFresh", "%1 adapters · %2 channels · %3 online", "%1 个适配器 · %2 个通道 · %3 个在线", "%1 Adapter · %2 Kanäle · %3 online", "アダプター %1 · チャンネル %2 · オンライン %3"},
    {"hardware.noEndpoints", "No UCAL endpoints discovered", "未发现 UCAL 端点", "Keine UCAL-Endpunkte gefunden", "UCAL エンドポイントが見つかりません"},
    {"hardware.noEndpointsHelp", "Connect supported hardware, check its driver, then rescan.", "请连接支持的硬件并检查驱动，然后重新扫描。", "Unterstützte Hardware anschließen, Treiber prüfen und erneut suchen.", "対応ハードウェアとドライバーを確認し、再スキャンしてください。"},
    {"software.eyebrow", "INSTALLED SOFTWARE", "软件安装实例", "INSTALLIERTE SOFTWARE", "インストール済みソフトウェア"},
    {"software.subtitle.explain", "Each entry is one path, version, and architecture; installations are not launch profiles.", "一条记录代表一个路径、版本和位数；软件安装本身不是可启动配置。", "Jeder Eintrag steht für Pfad, Version und Architektur; Installationen sind keine Startprofile.", "各項目はパス・バージョン・アーキテクチャを表し、起動プロファイルとは別です。"},
    {"software.subtitle.real", "5 software installations found on this computer", "在这台电脑上发现 5 个软件安装实例", "5 Softwareinstallationen auf diesem Computer gefunden", "このコンピューターで 5 件のソフトウェアを検出"},
    {"software.subtitle.production", "%1 software installations found", "已发现 %1 个软件安装实例", "%1 Softwareinstallationen gefunden", "%1 件のソフトウェアを検出"},
    {"software.addExe", "+ Add EXE", "+ 添加 EXE", "+ EXE hinzufügen", "+ EXE を追加"},
    {"software.createApp", "Create application", "创建应用", "Anwendung erstellen", "アプリを作成"},
    {"software.rescan", "Rescan", "重新探测", "Neu prüfen", "再検出"},
    {"software.dependencies", "Applications using this software", "使用此软件的应用", "Anwendungen dieser Installation", "このソフトを使うアプリ"},
    {"software.location", "Open location", "打开位置", "Ordner öffnen", "場所を開く"},
    {"software.scanRunning", "Scanning software…", "正在扫描软件…", "Software wird gesucht…", "ソフトウェアを検索中…"},
    {"software.scanComplete", "Scan complete · %1 installations found", "扫描完成 · 发现 %1 个软件安装实例", "Suche abgeschlossen · %1 Installationen", "スキャン完了 · %1 件を検出"},
    {"software.forget", "Forget this path", "忘记此路径", "Diesen Pfad vergessen", "このパスを削除"},
    {"software.compat.unknown", "Unsupported: no existing UCAL launch route recognizes this executable", "不支持：现有 UCAL 启动路线无法识别此 EXE", "Nicht unterstützt: Keine vorhandene UCAL-Startroute erkennt diese EXE", "未対応：既存の UCAL 起動経路ではこの EXE を認識できません"},
    {"software.compat.missing", "Unavailable: the executable is missing or unreadable", "不可用：EXE 不存在或无法读取", "Nicht verfügbar: Die EXE fehlt oder ist nicht lesbar", "利用不可：EXE が存在しないか読み取れません"},
    {"software.rescanComplete", "%1 compatibility was rechecked.", "已重新检查 %1 的兼容性。", "Die Kompatibilität von %1 wurde erneut geprüft.", "%1 の互換性を再確認しました。"},
    {"software.dependencies.none", "No application profile currently references this installation.", "当前没有应用配置引用此软件安装。", "Kein Anwendungsprofil verweist derzeit auf diese Installation.", "現在このインストールを参照するアプリプロファイルはありません。"},
    {"software.dependencies.used", "Referenced by:\n\n%1", "被以下应用配置引用：\n\n%1", "Verwendet von:\n\n%1", "次のアプリプロファイルが参照しています：\n\n%1"},
    {"software.forget.used", "Remove the referencing application profiles first:\n\n%1", "请先删除引用此软件安装的应用配置：\n\n%1", "Entfernen Sie zuerst die referenzierenden Anwendungsprofile:\n\n%1", "先に参照しているアプリプロファイルを削除してください：\n\n%1"},
    {"software.forget.question", "Remove this software record? Rescan can find it again.", "移除这条软件记录吗？不会卸载软件，重新扫描可以再次发现。", "Diesen manuell hinzugefügten Pfad vergessen?", "この手動追加パスを削除しますか？"},
    {"backends.eyebrow", "BACKEND PACKAGES", "后端包", "BACKEND-PAKETE", "バックエンドパッケージ"},
    {"backends.subtitle.explain", "Backend packages connect hardware to UCAL; built-in packages cannot be removed.", "后端包把硬件接入 UCAL；内置包不可删除，第三方包可独立管理。", "Backend-Pakete verbinden Hardware mit UCAL; integrierte Pakete sind geschützt.", "バックエンドパッケージがハードウェアを UCAL に接続します。"},
    {"backends.subtitle.real", "3 backends loaded · 1 optional package disabled", "3 个后端已加载 · 1 个可选包已禁用", "3 backends geladen · 1 optionales Paket deaktiviert", "3 バックエンド読込済み · オプション1件無効"},
    {"backends.add", "+ Register metadata", "+ 注册元数据", "+ Metadaten registrieren", "+ メタデータを登録"},
    {"backends.subtitle.production", "%1 extension registrations · live Broker backends below", "%1 个扩展注册项 · 下方显示当前 Broker 后端", "%1 Erweiterungsregistrierungen · aktuelle Broker-backends unten", "拡張登録 %1 件 · 現在の Broker バックエンドを以下に表示"},
    {"backends.registerTitle", "Register UCAL backend metadata", "注册 UCAL 后端元数据", "UCAL-Backend-Metadaten registrieren", "UCAL バックエンドメタデータを登録"},
    {"backends.manifestFilter", "UCAL backend manifest (*.json)", "UCAL 后端清单 (*.json)", "UCAL-Backend-Manifest (*.json)", "UCAL バックエンドマニフェスト (*.json)"},
    {"backends.persistFailed", "The backend registration could not be persisted.", "无法保存后端注册信息。", "Die Backend-Registrierung konnte nicht gespeichert werden.", "バックエンド登録を保存できませんでした。"},
    {"backends.revalidated", "Backend registration revalidated: %1", "已重新校验后端注册：%1", "Backend-Registrierung erneut geprüft: %1", "バックエンド登録を再検証しました：%1"},
    {"backends.registered", "Backend registration added: %1", "已添加后端注册：%1", "Backend-Registrierung hinzugefügt: %1", "バックエンド登録を追加しました：%1"},
    {"backends.builtinDetails", "This is a built-in UCAL backend. Switch reports its state but does not copy, remove, or modify its vendor runtime or driver.\n\nState: %1\nCapabilities: %2", "这是内置 UCAL 后端。Switch 仅报告其状态，不会复制、删除或修改厂商运行库或驱动。\n\n状态：%1\n能力：%2", "Dies ist ein integriertes UCAL-Backend. Switch zeigt seinen Zustand an, kopiert, entfernt oder verändert jedoch weder Herstellerlaufzeit noch Treiber.\n\nStatus: %1\nFunktionen: %2", "これは組み込み UCAL バックエンドです。Switch は状態のみ表示し、ベンダーランタイムやドライバーのコピー、削除、変更は行いません。\n\n状態：%1\n機能：%2"},
    {"backends.invalid", "Backend registration invalid", "后端注册无效", "Backend-Registrierung ungültig", "バックエンド登録が無効です"},
    {"backends.matches", "Registration and live Broker capabilities match.", "注册信息与 Broker 当前能力一致。", "Registrierung und aktuelle Broker-Funktionen stimmen überein.", "登録情報と現在の Broker 機能が一致しています。"},
    {"backends.unavailable", "Registration is valid, but its backend family or capabilities are not currently available.", "注册信息有效，但对应后端系列或能力当前不可用。", "Die Registrierung ist gültig, aber die Backend-Familie oder ihre Funktionen sind derzeit nicht verfügbar.", "登録は有効ですが、対応するバックエンド系列または機能は現在利用できません。"},
    {"backends.registrationDetails", "Family: %1\nCapabilities: %2\nSHA-256: %3\nRecord: %4", "系列：%1\n能力：%2\nSHA-256：%3\n记录：%4", "Familie: %1\nFunktionen: %2\nSHA-256: %3\nEintrag: %4", "系列：%1\n機能：%2\nSHA-256：%3\n登録：%4"},
    {"backends.remove", "Remove registration", "移除注册", "Registrierung entfernen", "登録を削除"},
    {"backends.removeQuestion", "Remove the user-workspace registration for %1?\n\nNo runtime or driver file will be changed.", "移除工作区中 %1 的注册吗？\n\n不会改动任何运行库或驱动文件。", "Die Benutzer-Arbeitsbereichsregistrierung für %1 entfernen?\n\nLaufzeit- und Treiberdateien bleiben unverändert.", "%1 のユーザーワークスペース登録を削除しますか？\n\nランタイムやドライバーファイルは変更されません。"},
    {"backends.removeFailed", "The registration could not be removed.", "无法移除注册。", "Die Registrierung konnte nicht entfernt werden.", "登録を削除できませんでした。"},
    {"backends.removed", "Backend registration removed: %1", "已移除后端注册：%1", "Backend-Registrierung entfernt: %1", "バックエンド登録を削除しました：%1"},
    {"backends.activationAvailable", "The live Broker reports the declared family and capabilities.", "当前 Broker 已报告声明的后端系列和能力。", "Der aktive Broker meldet die angegebene Familie und die angegebenen Funktionen.", "現在の Broker は宣言された系列と機能を報告しています。"},
    {"backends.activationUnavailable", "The registration remains inactive until a live Broker endpoint reports the declared family and every requested capability.", "在 UCAL 通道报告声明的后端系列及全部所需能力之前，此注册保持未激活。", "Die Registrierung bleibt inaktiv, bis ein aktiver Broker-Endpunkt die angegebene Familie und alle angeforderten Funktionen meldet.", "Broker エンドポイントが宣言された系列と必要な全機能を報告するまで、この登録は無効のままです。"},
    {"backends.registrationSummary", "Name: %1\nVendor: %2\nVersion: %3\nArchitecture: %4\nBackend family: %5\nCapabilities: %6\nSHA-256: %7\n\n%8\n\nOnly this manifest is copied into the user workspace. No vendor runtime, driver, or DLL is installed.", "名称：%1\n厂商：%2\n版本：%3\n架构：%4\n后端系列：%5\n能力：%6\nSHA-256：%7\n\n%8\n\n仅将此清单复制到用户工作区；不会安装厂商运行库、驱动或 DLL。", "Name: %1\nHersteller: %2\nVersion: %3\nArchitektur: %4\nBackend-Familie: %5\nFunktionen: %6\nSHA-256: %7\n\n%8\n\nNur dieses Manifest wird in den Benutzer-Arbeitsbereich kopiert. Herstellerlaufzeit, Treiber und DLLs werden nicht installiert.", "名前：%1\nベンダー：%2\nバージョン：%3\nアーキテクチャ：%4\nバックエンド系列：%5\n機能：%6\nSHA-256：%7\n\n%8\n\nこのマニフェストだけをユーザーワークスペースへコピーします。ベンダーランタイム、ドライバー、DLL はインストールしません。"},
    {"backends.confirm", "Confirm backend registration", "确认后端注册", "Backend-Registrierung bestätigen", "バックエンド登録を確認"},
    {"settings.eyebrow", "PREFERENCES", "偏好设置", "EINSTELLUNGEN", "設定"},
    {"settings.subtitle.explain", "System behavior and product information.", "系统行为与产品信息。", "Systemverhalten und Produktinformationen.", "システム動作と製品情報。"},
    {"settings.subtitle.real", "Startup, language, updates, and product information", "启动、语言、更新与产品信息", "Start, Sprache, Updates und Produktinformationen", "起動、言語、更新、製品情報"},
    {"settings.startup", "Startup & tray", "启动与托盘", "Start & Infobereich", "起動とトレイ"},
    {"settings.autostart", "Start UCAL Manager after Windows sign-in", "Windows 登录后启动 UCAL Manager", "UCAL Manager nach Windows-Anmeldung starten", "Windows ログイン後に UCAL Manager を起動"},
    {"settings.startMin", "Start minimized to tray", "启动后直接最小化到托盘", "Minimiert im Infobereich starten", "起動時にトレイへ最小化"},
    {"settings.autoScan", "Scan software and hardware at startup", "启动时自动扫描软件和硬件", "Software und Hardware beim Start suchen", "起動時にソフトとハードをスキャン"},
    {"settings.autoUpdate", "Automatically check for updates", "自动检查更新", "Automatisch nach Updates suchen", "更新を自動確認"},
    {"settings.updateUnavailable", "No update source configured yet.", "尚未配置更新源。", "Noch keine Updatequelle konfiguriert.", "更新元は未設定です。"},
    {"settings.closeButton", "When closing the window", "点击关闭按钮时", "Beim Schließen des Fensters", "ウィンドウを閉じるとき"},
    {"settings.hideTray", "Hide to tray", "隐藏到托盘", "In Infobereich ausblenden", "トレイに隠す"},
    {"settings.ask", "Ask every time", "每次询问", "Jedes Mal fragen", "毎回確認"},
    {"settings.exit", "Exit", "直接退出", "Beenden", "終了"},
    {"settings.language", "Language", "语言", "Sprache", "言語"},
    {"settings.languageHelp", "The interface restarts after changing the language.", "切换语言后界面会立即重新加载。", "Nach einem Sprachwechsel wird die Oberfläche neu geladen.", "言語変更後、画面を再読み込みします。"},
    {"language.system", "Follow system", "跟随系统", "Systemsprache verwenden", "システム設定に従う"},
    {"settings.localConfig", "Local configuration & diagnostics", "本地配置与诊断", "Lokale Konfiguration & Diagnose", "ローカル設定と診断"},
    {"settings.savedLocally", "Projects, applications, mappings, and preferences are stored locally.", "项目、应用、映射和偏好设置保存在本机。", "Projekte, Anwendungen, Zuordnungen und Einstellungen werden lokal gespeichert.", "プロジェクト、アプリ、割り当て、設定はローカルに保存されます。"},
    {"editor.title", "Application settings · %1", "应用设置 · %1", "Anwendungseinstellungen · %1", "アプリ設定 · %1"},
    {"editor.basic", "General", "基本信息", "Allgemein", "基本情報"},
    {"editor.channels", "Hardware channels", "硬件通道", "Hardwarekanäle", "ハードウェアチャンネル"},
    {"editor.integration", "Integration & launch", "接入与启动", "Integration & Start", "統合と起動"},
    {"editor.name", "Application name", "应用名称", "Anwendungsname", "アプリ名"},
    {"editor.installation", "Software installation", "软件安装", "Softwareinstallation", "ソフトウェアインストール"},
    {"editor.projects", "Projects", "所属项目", "Projekte", "プロジェクト"},
    {"editor.projectHelp", "Select projects in Project Management; the default project always includes this application.", "在“管理项目”中选择；默认项目始终包含此应用。", "Projekte in der Projektverwaltung wählen; das Standardprojekt enthält diese Anwendung immer.", "プロジェクト管理で選択します。デフォルトプロジェクトには常に含まれます。"},
    {"editor.full", "All compatible channels", "全部兼容通道", "Alle kompatiblen Kanäle", "全互換チャンネル"},
    {"editor.selected", "Selected channels", "指定通道", "Ausgewählte Kanäle", "指定チャンネル"},
    {"editor.fullDetail", "All compatible channels", "全部兼容通道", "Alle kompatiblen Kanäle", "全互換チャンネル"},
    {"editor.selectedDetail", "Selected channels", "指定通道", "Ausgewählte Kanäle", "指定チャンネル"},
    {"editor.dynamicNote", "Assigns current compatible channels in stable order at launch, up to %1. Use selected channels for fixed bindings.", "启动时按稳定顺序自动提供当前兼容通道，最多 %1 个。需要固定绑定时，请选择“指定通道”。", "Weist beim Start kompatible Kanäle in stabiler Reihenfolge zu, höchstens %1. Für feste Zuordnungen Kanäle auswählen.", "起動時に対応チャンネルを安定した順序で最大 %1 個割り当てます。固定割り当てには個別指定を使用します。"},
    {"editor.selectedNote", "The list order is the application channel order; offline endpoints keep their slots.", "列表顺序就是应用通道顺序；离线端点保留原槽位。", "Die Listenreihenfolge entspricht den Anwendungskanälen; Offline-Endpunkte behalten ihren Platz.", "一覧順がアプリチャンネル順です。オフラインでも位置を保持します。"},
    {"editor.logicalChannel", "Application channel", "应用通道", "Anwendungskanal", "アプリチャンネル"},
    {"editor.endpoint", "Hardware endpoint", "硬件端点", "Hardware-Endpunkt", "ハードウェアエンドポイント"},
    {"editor.state", "Status", "状态", "Status", "状態"},
    {"editor.unassigned", "Unassigned", "未分配", "Nicht zugewiesen", "未割り当て"},
    {"editor.integrationMethod", "Integration", "接入方式", "Integration", "統合方式"},
    {"editor.native", "Native plugin / UCAL API", "原生插件 / UCAL API", "Natives Plugin / UCAL API", "ネイティブプラグイン / UCAL API"},
    {"editor.evidence", "Compatibility", "兼容状态", "Kompatibilität", "互換状態"},
    {"editor.connection", "Connection", "连接状态", "Verbindung", "接続状態"},
    {"editor.adapterId", "Selected adapter", "已选接入适配器", "Ausgewählter Adapter", "選択したアダプター"},
    {"editor.routeScope", "Evidence scope", "证据范围", "Nachweisumfang", "証拠の範囲"},
    {"editor.route.native", "Native application route; UCAL mapping is not applied.", "原生应用路线；不会应用 UCAL 映射。", "Native Anwendungsroute; die UCAL-Zuordnung wird nicht angewendet.", "ネイティブ起動経路です。UCAL マッピングは適用されません。"},
    {"editor.route.unknown", "No accepted product route is identified for this adapter.", "尚未识别出此适配器对应的已验收产品路线。", "Für diesen Adapter ist keine abgenommene Produktroute bekannt.", "このアダプターに対応する受け入れ済み製品経路は確認されていません。"},
    {"editor.whyPath", "Why PATH alone is not enough", "为什么不能只修改 PATH？", "Warum PATH allein nicht genügt", "PATH だけでは不十分な理由"},
    {"editor.pathExplanation", "Windows searches the EXE directory before PATH. UCAL therefore probes the actual loading contract before choosing an integration adapter.", "Windows 会先搜索 EXE 目录，因此 UCAL 会先探测实际加载契约，再选择接入适配器。", "Windows durchsucht das EXE-Verzeichnis vor PATH. UCAL prüft deshalb zuerst den tatsächlichen Ladevertrag.", "Windows は PATH より先に EXE ディレクトリを検索するため、UCAL は実際の読み込み条件を確認します。"},
    {"editor.save", "Save application", "保存应用", "Anwendung speichern", "アプリを保存"},
    {"status.summary", "%1 applications · %2 hardware channels · %3 online", "%1 个应用 · %2 个硬件通道 / %3 在线", "%1 Anwendungen · %2 Hardwarekanäle · %3 online", "%1 アプリ · %2 ハードウェアチャンネル · %3 オンライン"},
    {"status.scanComplete", "Scan complete · inventory updated", "扫描完成 · 清单已更新", "Suche abgeschlossen · Bestand aktualisiert", "スキャン完了 · 一覧を更新しました"},
    {"status.scanInProgress", "Scanning…", "正在扫描……", "Suche läuft…", "スキャン中…"},
    {"status.connectingBroker", "Connecting to UCAL…", "正在连接 UCAL……", "Verbindung mit UCAL…", "UCAL に接続中…"},
    {"status.brokerUnavailable", "UCAL service unavailable", "UCAL 服务不可用", "UCAL-Dienst nicht verfügbar", "UCAL サービスを利用できません"},
    {"status.mappingsPreserved", "%1 · saved mappings preserved offline", "%1 · 已保存映射以离线占位保留", "%1 · gespeicherte Zuordnungen bleiben offline erhalten", "%1 · 保存済み割り当てをオフライン位置として保持"},
    {"status.brokerOnlineEmpty", "Connected · no hardware channels found", "已连接 · 未找到硬件通道", "Verbunden · keine Hardwarekanäle gefunden", "接続済み · チャンネルなし"},
    {"status.inventoryRefreshed", "Scan complete · %1 channels", "扫描完成 · %1 个通道", "Suche abgeschlossen · %1 Kanäle", "スキャン完了 · %1 チャンネル"},
    {"status.partialBackendFailure", "Scan complete · some drivers need attention: %1", "扫描完成 · 部分驱动需要检查：%1", "Suche abgeschlossen · Treiber prüfen: %1", "スキャン完了 · ドライバーを確認：%1"},
    {"tray.currentProject", "Current project: %1", "当前项目：%1", "Aktuelles Projekt: %1", "現在のプロジェクト：%1"},
    {"tray.launchApp", "Launch %1", "启动 %1", "%1 starten", "%1 を起動"},
    {"tray.rescan", "Rescan hardware", "重新扫描硬件", "Hardware neu suchen", "ハードウェアを再スキャン"},
    {"about.build", "One interface. Any CAN ecosystem. · Built %1 %2", "一个接口，连接任意 CAN 生态。 · 构建 %1 %2", "Eine Schnittstelle. Jedes CAN-Ökosystem. · Build %1 %2", "1つのインターフェースで、あらゆる CAN エコシステムへ。 · ビルド %1 %2"},
    {"about.githubLicense", "Project and license information", "项目与许可信息", "Projekt- und Lizenzinformationen", "プロジェクトとライセンス情報"},
    {"about.githubPoc", "GitHub project page (available after productization) · Open-source license", "GitHub 项目主页（产品化阶段启用） · 开源许可证", "GitHub-Projektseite (nach Produktreife verfügbar) · Open-Source-Lizenz", "GitHub プロジェクトページ（製品化後に利用可能） · オープンソースライセンス"},
    {"about.community", "Community Edition", "社区版", "Community-Ausgabe", "コミュニティ版"},
    {"about.interactionPoc", "Interaction POC", "交互 POC", "Interaktions-POC", "インタラクション POC"},
    {"common.save", "Save", "保存", "Speichern", "保存"},
    {"common.new", "New", "新建", "Neu", "新規"},
    {"common.rename", "Rename", "重命名", "Umbenennen", "名前を変更"},
    {"common.delete", "Delete", "删除", "Löschen", "削除"},
    {"common.done", "Done", "完成", "Fertig", "完了"},
    {"common.softwareUnavailable", "Software installation unavailable", "软件安装不可用", "Softwareinstallation nicht verfügbar", "ソフトウェアのインストールが利用できません"},
    {"apps.copySuffix", " copy", " 副本", " Kopie", " コピー"},
    {"apps.delete", "Delete application", "删除应用", "Anwendung löschen", "アプリを削除"},
    {"apps.deleteQuestion", "Delete “%1”?\n\nThe software installation and other applications will not be deleted.", "删除“%1”？\n\n软件安装和其他应用不会被删除。", "„%1“ löschen?\n\nDie Softwareinstallation und andere Anwendungen bleiben erhalten.", "「%1」を削除しますか？\n\nソフトウェアのインストールと他のアプリは削除されません。"},
    {"apps.create", "Create application", "创建应用", "Anwendung erstellen", "アプリを作成"},
    {"apps.applicationName", "Application name", "应用名称", "Anwendungsname", "アプリ名"},
    {"project.managerTitle", "Manage projects", "管理项目", "Projekte verwalten", "プロジェクト管理"},
    {"project.managerHelp", "Select the applications in each custom project on the right. The default project always includes every application.", "在右侧勾选自建项目包含的应用。默认项目始终汇总全部应用。", "Rechts die Anwendungen für eigene Projekte auswählen. Das Standardprojekt enthält immer alle Anwendungen.", "独自プロジェクトのアプリを右側で選択します。既定プロジェクトにはすべてのアプリが含まれます。"},
    {"project.allSuffix", " · All applications", " · 全部应用", " · Alle Anwendungen", " · すべてのアプリ"},
    {"project.newTitle", "New project", "新建项目", "Neues Projekt", "新規プロジェクト"},
    {"project.renameTitle", "Rename project", "重命名项目", "Projekt umbenennen", "プロジェクト名を変更"},
    {"project.name", "Project name", "项目名称", "Projektname", "プロジェクト名"},
    {"addApp.title", "Add application", "添加应用", "Anwendung hinzufügen", "アプリを追加"},
    {"addApp.heading", "Create an application profile from a software installation", "从一份软件安装创建应用配置", "Anwendungsprofil aus einer Softwareinstallation erstellen", "ソフトウェアインストールからアプリプロファイルを作成"},
    {"addApp.help", "Known software can be discovered automatically. Select an EXE for in-house, portable, or demo software.", "已知软件可自动扫描；公司自研、绿色版或 Demo 可手动选择 EXE。", "Bekannte Software kann automatisch gefunden werden. Für interne, portable oder Demo-Software eine EXE auswählen.", "既知のソフトウェアは自動検出できます。自社製、ポータブル、デモ版は EXE を選択してください。"},
    {"addApp.installation", "Software installation", "软件安装", "Softwareinstallation", "ソフトウェアのインストール"},
    {"addApp.path", "Path", "路径", "Pfad", "パス"},
    {"addApp.browse", "Select EXE manually…", "手动选择 EXE…", "EXE manuell auswählen…", "EXE を手動で選択…"},
    {"addApp.note", "Selecting an EXE creates a software record. It is compatible only when an existing UCAL launch route recognizes its version and architecture.", "选择 EXE 会创建软件记录；只有现有 UCAL 启动路线识别其版本和架构时才标为兼容。", "Die EXE-Auswahl erstellt einen Softwareeintrag. Kompatibel ist er nur, wenn eine vorhandene UCAL-Startroute Version und Architektur erkennt.", "EXE を選択するとソフトウェア情報を作成します。既存の UCAL 起動経路がバージョンとアーキテクチャを認識した場合のみ互換と表示します。"},
    {"addApp.create", "Create and configure", "创建并设置", "Erstellen und konfigurieren", "作成して設定"},
    {"addApp.selectExe", "Select application", "选择应用程序", "Anwendung auswählen", "アプリケーションを選択"},
    {"addApp.exeFilter", "Windows applications (*.exe)", "Windows 应用程序 (*.exe)", "Windows-Anwendungen (*.exe)", "Windows アプリケーション (*.exe)"},
    {"addApp.errorTitle", "Cannot create application", "无法创建", "Anwendung kann nicht erstellt werden", "アプリを作成できません"},
    {"addApp.errorMessage", "Select a software installation and enter an application name.", "请选择软件并填写应用名称。", "Softwareinstallation auswählen und einen Anwendungsnamen eingeben.", "ソフトウェアを選択し、アプリ名を入力してください。"},
    {"hardware.assignTitle", "Applications using this channel", "使用此通道的应用", "Anwendungen dieses Kanals", "このチャンネルを使うアプリ"},
    {"hardware.visibilityHelp", "This sets visibility. UCAL checks channel availability when an application connects.", "这里设置通道可见性；应用连接时由 UCAL 判定通道是否可用。", "Dies legt die Sichtbarkeit fest. UCAL prüft die Verfügbarkeit beim Verbinden.", "表示対象を設定します。接続時に UCAL が利用可否を確認します。"},
    {"hardware.fullTooltip", "All-compatible-channels mode automatically sees this endpoint. Switch the application to selected channels before excluding it.", "全部兼容通道模式会自动看见此端点；如需排除，请先在应用设置中改为指定通道。", "Im Modus „Alle kompatiblen Kanäle“ ist dieser Endpunkt automatisch sichtbar. Zum Ausschließen zuerst auf ausgewählte Kanäle umstellen.", "「すべての互換チャンネル」ではこのエンドポイントが自動表示されます。除外するには選択チャンネルへ切り替えてください。"},
    {"hardware.fullHelp", "Disabled applications use all compatible channels. Switch them to selected channels before excluding this endpoint.", "灰色应用处于“全部兼容通道”模式；需先转换为“指定通道”才能排除此端点。", "Deaktivierte Anwendungen verwenden alle kompatiblen Kanäle. Vor dem Ausschließen auf ausgewählte Kanäle umstellen.", "無効表示のアプリはすべての互換チャンネルを使用します。除外するには選択チャンネルへ切り替えてください。"},
    {"tray.running", "UCAL Manager is still running in the tray. Double-click the icon to restore it.", "UCAL Manager 仍在托盘运行；双击图标可恢复。", "UCAL Manager läuft im Infobereich weiter. Zum Wiederherstellen das Symbol doppelklicken.", "UCAL Manager はトレイで実行中です。アイコンをダブルクリックすると復元します。"},
    {"tray.exitTitle", "Exit UCAL Manager", "退出 UCAL Manager", "UCAL Manager beenden", "UCAL Manager を終了"},
    {"tray.exitQuestion", "Exit the application? Choose No to return to the main window.", "退出程序？选择“否”将返回主窗口。", "Anwendung beenden? Mit Nein kehren Sie zum Hauptfenster zurück.", "アプリを終了しますか？「いいえ」を選ぶとメイン画面に戻ります。"},
};

const TranslationRow* findTranslation(const QString& key) {
    for (const TranslationRow& row : kTranslations) {
        if (key == QLatin1String(row.key)) return &row;
    }
    return nullptr;
}

QString preferencePath() {
    const auto requested = qEnvironmentVariable("UCAL_MANAGER_SETTINGS_DIRECTORY");
    const QString directory = portableDataDirectory();
    Q_UNUSED(requested);
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("preferences.ini"));
}

}  // namespace

PresentationMode presentationModeFromArguments(const QStringList& arguments,
                                               const QString& executablePath) {
    const int option = arguments.indexOf(QStringLiteral("--presentation"));
    if (option >= 0 && option + 1 < arguments.size()) {
        const QString requested = arguments.at(option + 1);
        if (requested.compare(QStringLiteral("realistic"), Qt::CaseInsensitive) == 0) {
            return PresentationMode::Realistic;
        }
        if (requested.compare(QStringLiteral("explanatory"), Qt::CaseInsensitive) == 0) {
            return PresentationMode::Explanatory;
        }
        return PresentationMode::Production;
    }
    const QString name = QFileInfo(executablePath).completeBaseName();
    if (name.contains(QStringLiteral("realistic"), Qt::CaseInsensitive)) {
        return PresentationMode::Realistic;
    }
    if (name.contains(QStringLiteral("explanatory"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("poc"), Qt::CaseInsensitive)) {
        return PresentationMode::Explanatory;
    }
    return PresentationMode::Production;
}

QString presentationModeKey(const PresentationMode mode) {
    if (mode == PresentationMode::Realistic) return QStringLiteral("realistic");
    if (mode == PresentationMode::Explanatory) return QStringLiteral("explanatory");
    return QStringLiteral("production");
}

UiLanguage uiLanguageFromKey(const QString& key) {
    if (key.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0) return UiLanguage::English;
    if (key.compare(QStringLiteral("zh-CN"), Qt::CaseInsensitive) == 0 || key == QStringLiteral("zh")) return UiLanguage::SimplifiedChinese;
    if (key.compare(QStringLiteral("de"), Qt::CaseInsensitive) == 0) return UiLanguage::German;
    if (key.compare(QStringLiteral("ja"), Qt::CaseInsensitive) == 0) return UiLanguage::Japanese;
    return UiLanguage::System;
}

QString uiLanguageKey(const UiLanguage language) {
    switch (language) {
    case UiLanguage::English: return QStringLiteral("en");
    case UiLanguage::SimplifiedChinese: return QStringLiteral("zh-CN");
    case UiLanguage::German: return QStringLiteral("de");
    case UiLanguage::Japanese: return QStringLiteral("ja");
    case UiLanguage::System: return QStringLiteral("system");
    }
    return QStringLiteral("system");
}

UiLanguage resolvedUiLanguage(const UiLanguage configuredLanguage) {
    if (configuredLanguage != UiLanguage::System) return configuredLanguage;
    switch (QLocale::system().language()) {
    case QLocale::Chinese: return UiLanguage::SimplifiedChinese;
    case QLocale::German: return UiLanguage::German;
    case QLocale::Japanese: return UiLanguage::Japanese;
    default: return UiLanguage::English;
    }
}

QString uiLanguageOptionText(const UiLanguage option, const UiLanguage configuredLanguage) {
    switch (option) {
    case UiLanguage::System: return uiText(configuredLanguage, QStringLiteral("language.system"));
    case UiLanguage::English: return QStringLiteral("English");
    case UiLanguage::SimplifiedChinese: return QStringLiteral("简体中文");
    case UiLanguage::German: return QStringLiteral("Deutsch");
    case UiLanguage::Japanese: return QStringLiteral("日本語");
    }
    return QString();
}

UiLanguage loadUiLanguagePreference() {
    QSettings settings(preferencePath(), QSettings::IniFormat);
    return uiLanguageFromKey(settings.value(QStringLiteral("ui/language"), QStringLiteral("system")).toString());
}

bool saveUiLanguagePreference(const UiLanguage language) {
    QSettings settings(preferencePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ui/language"), uiLanguageKey(language));
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QString uiLanguagePreferencePath() {
    return preferencePath();
}

QString uiText(const UiLanguage configuredLanguage, const QString& key) {
    const TranslationRow* row = findTranslation(key);
    if (row == nullptr) return QStringLiteral("[%1]").arg(key);
    switch (resolvedUiLanguage(configuredLanguage)) {
    case UiLanguage::SimplifiedChinese: return QString::fromUtf8(row->zh);
    case UiLanguage::German: return QString::fromUtf8(row->de);
    case UiLanguage::Japanese: return QString::fromUtf8(row->ja);
    case UiLanguage::English:
    case UiLanguage::System: return QString::fromUtf8(row->en);
    }
    return QString::fromUtf8(row->en);
}

QString duplicateEndpointMessage(const UiLanguage language, const QVector<int>& channels) {
    if (channels.isEmpty()) return {};
    QStringList names;
    for (const int channel : channels) names << QStringLiteral("CAN %1").arg(channel);
    return uiText(language, QStringLiteral("mapping.duplicateEndpoint"))
        .arg(names.join(uiText(language, QStringLiteral("mapping.channelListSeparator"))));
}

void installStandardButtonTranslations(const UiLanguage language) {
    static StandardButtonTranslator translator;
    QCoreApplication::removeTranslator(&translator);
    translator.language = language;
    QCoreApplication::installTranslator(&translator);
}

}  // namespace Ucal::Manager
