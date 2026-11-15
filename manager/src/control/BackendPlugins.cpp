#include "BackendPlugins.h"
#include "ProductOperations.h"
#include "UiPresentation.h"
#include "PluginMetadata.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QUuid>

namespace Ucal::Manager {
namespace {
bool scalar(const QString &s, int maximum, bool empty = false) {
    return (empty || !s.isEmpty()) && s.toUtf8().size() <= maximum && !s.contains('\r') &&
           !s.contains('\n') && !s.contains(QChar(0));
}
bool identifier(const QString &s) {
    return s.size() < 128 && QRegularExpression(QStringLiteral("^[A-Za-z0-9._-]+$")).match(s).hasMatch();
}
QString resolved(const QString &path, const QString &manifest) {
    return path.isEmpty()
               ? QString()
               : QDir::cleanPath(QFileInfo(QFileInfo(path).isAbsolute()
                                               ? path
                                               : QDir(QFileInfo(manifest).absolutePath()).filePath(path))
                                     .absoluteFilePath());
}
bool integer(const QJsonValue &value, quint32 &result, quint32 maximum) {
    if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > maximum ||
        value.toDouble() != static_cast<quint32>(value.toDouble()))
        return false;
    result = static_cast<quint32>(value.toDouble());
    return true;
}
bool channelsValid(const QVector<BackendChannelBinding> &channels, QString *error) {
    QSet<QString> identities, selectors, physicalPorts;
    if (channels.size() > 64) {
        if (error)
            *error = QStringLiteral("At most 64 bindings are supported");
        return false;
    }
    for (const auto &c : channels) {
        if (!identifier(c.identity) || !scalar(c.name, 127) || !scalar(c.selector, 512, true) ||
            c.physicalPort > 255 || identities.contains(c.identity) || selectors.contains(c.selector) ||
            (!c.adapterIdentity.isEmpty() &&
             (!identifier(c.adapterIdentity) || physicalPorts.contains(c.adapterIdentity + QLatin1Char('/') +
                                                                       QString::number(c.physicalPort))))) {
            if (error)
                *error = QStringLiteral("Channel names, identities and selectors must be valid and unique; "
                                        "only one default selector is allowed");
            return false;
        }
        identities.insert(c.identity);
        selectors.insert(c.selector);
        if (!c.adapterIdentity.isEmpty())
            physicalPorts.insert(c.adapterIdentity + QLatin1Char('/') + QString::number(c.physicalPort));
    }
    return true;
}
bool atomicText(const QString &path, const QString &text, QString *error) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("Cannot create plugin settings folder");
        return false;
    }
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        QTextStream stream(&existing);
        stream.setAutoDetectUnicode(true);
        stream.setCodec("UTF-8");
        if (stream.readAll() == text)
            return true;
    }
    existing.close();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QTextStream stream(&file);
    stream.setCodec("UTF-16LE");
    stream.setGenerateByteOrderMark(true);
    stream << text;
    stream.flush();
    if (stream.status() != QTextStream::Ok || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
QString runtimeAsset(const QString &applicationDirectory, const QString &arch, const QString &file) {
    QStringList roots{QFileInfo(backendDirectory(applicationDirectory)).absolutePath()};
    QDir directory(applicationDirectory);
    for (int depth = 0; depth < 8; ++depth) {
        roots << directory.absolutePath();
        if (!directory.cdUp())
            break;
    }
    for (const auto &root : roots)
        for (const auto &relative :
             {QStringLiteral("runtime/%1/%2").arg(arch, file),
              QStringLiteral("out/%1/Release/%2")
                  .arg(arch == QStringLiteral("x86") ? QStringLiteral("Win32") : QStringLiteral("x64"),
                       file)}) {
            const auto path = QDir(root).filePath(relative);
            if (QFileInfo(path).isFile())
                return QFileInfo(path).absoluteFilePath();
        }
    return {};
}
bool migrateNativeConfiguration(BackendPackage& package, const QString& settings, QString* error) {
    const auto target = QDir(settings).filePath(package.id + QStringLiteral(".ini"));
    auto source = package.configurationTemplatePath;
    if (source.isEmpty()) source = QDir(QFileInfo(package.manifestPath).absolutePath())
                                      .filePath(QStringLiteral("backend.ini"));
    package.configurationPath = target;
    // Existing user settings always win; never rewrite a module's installed files.
    if (QFileInfo::exists(target)) {
        QFile saved(target);
        if (!QFileInfo(target).isFile() || !saved.open(QIODevice::ReadOnly) || saved.size() > 262144) {
            *error = QStringLiteral("Cannot read backend user settings"); return false;
        }
        return true;
    }
    if (!QFileInfo::exists(source) || source == target) return true;
    QFile legacy(source);
    if (!legacy.open(QIODevice::ReadOnly) || legacy.size() > 262144) {
        *error = QStringLiteral("Cannot read legacy backend settings"); return false;
    }
    QTextStream stream(&legacy); stream.setAutoDetectUnicode(true); stream.setCodec("UTF-8");
    auto lines = stream.readAll().split(QLatin1Char('\n'));
    QString section;
    bool hasSettings = false;
    for (auto& line : lines) {
        const auto text = line.trimmed();
        if (text.startsWith(QLatin1Char('[')) && text.endsWith(QLatin1Char(']'))) {
            section = text.mid(1, text.size()-2).toLower(); continue;
        }
        if (text.isEmpty() || text.startsWith(QLatin1Char(';')) || text.startsWith(QLatin1Char('#'))) continue;
        const auto at = line.indexOf(QLatin1Char('='));
        if (at <= 0) continue;
        const auto key = line.left(at).trimmed().toLower();
        auto value = line.mid(at+1).trimmed();
        if (section == QStringLiteral("runtime") && key == QStringLiteral("library")) {
            // Match Windows INI quote handling before resolving an old relative path.
            if (value.size() >= 2 && (value.front() == QLatin1Char('"') || value.front() == QLatin1Char('\'')) &&
                value.back() == value.front()) value = value.mid(1, value.size()-2);
            if (value.isEmpty()) continue;
            // Preserve the meaning of an old path relative to the module directory.
            line = QStringLiteral("library=") + QDir::toNativeSeparators(resolved(value, source));
        }
        hasSettings = true;
    }
    return !hasSettings || atomicText(target, lines.join(QLatin1Char('\n')), error);
}
} // namespace
bool parseBackendPlugin(const QJsonObject &input, const QString &path, BackendPackage &p, QString &error) {
    error.clear();
    QJsonValue normalized;
    if (!convertPluginJson(input, &normalized, false, &error)) return false;
    const auto o = normalized.toObject();
    p = {};
    const auto fail = [&]() {
        error = QStringLiteral("Invalid backend plugin description");
        return false;
    };
    const QSet<QString> keys{QStringLiteral("schema"),        QStringLiteral("id"),
                             QStringLiteral("name"), QStringLiteral("description"), QStringLiteral("vendor"),
                             QStringLiteral("version"),       QStringLiteral("mode"),
                             QStringLiteral("library"),       QStringLiteral("backendFamily"),
                             QStringLiteral("configuration"), QStringLiteral("capabilities"),
                             QStringLiteral("channels"),      QStringLiteral("fixedTiming"),
                             QStringLiteral("pins"), QStringLiteral("type"), QStringLiteral("configurationTool"), QStringLiteral("preserveEndpointIdentity"), QStringLiteral("discoveryLibrary"), QStringLiteral("brokerAliases"), QStringLiteral("isolated"), QStringLiteral("endpointPresentation"), QStringLiteral("launch")};
    if (o.contains("launch") && !o.value("launch").isObject()) return fail();
    if (o.contains("brokerAliases") && !o.value("brokerAliases").isArray()) return fail();
    for (const auto& value : o.value("brokerAliases").toArray())
        if (!value.isString() || !identifier(value.toString())) return fail();
    if (o.contains("isolated") && !o.value("isolated").isBool()) return fail();
    if (o.contains("endpointPresentation")) {
        if (!o.value("endpointPresentation").isObject()) return fail();
        const auto display = o.value("endpointPresentation").toObject();
        for (auto i = display.begin(); i != display.end(); ++i) {
            if (i.key() == "family" || i.key() == "fallbackDescription") {
                if (!i.value().isString() || !scalar(i.value().toString(), 128)) return fail();
            } else if (i.key() == "handleFirst" || i.key() == "handleLast" || i.key() == "handleOffset") {
                quint32 n; if (!integer(i.value(), n, 65535)) return fail();
            } else return fail();
        }
        if (display.contains("handleFirst") || display.contains("handleLast") || display.contains("handleOffset")) {
            if (!display.contains("handleFirst") || !display.contains("handleLast") || !display.contains("handleOffset") ||
                display.value("handleFirst").toInt() <= display.value("handleOffset").toInt() ||
                display.value("handleLast").toInt() < display.value("handleFirst").toInt()) return fail();
        }
    }
    for (auto i = o.begin(); i != o.end(); ++i)
        if (!keys.contains(i.key()))
            return fail();
    for (const auto &key : {"schema", "id", "name", "vendor", "version", "mode", "library", "backendFamily",
                            "configuration", "capabilities"})
        if (o.contains(QLatin1String(key)) && !o.value(QLatin1String(key)).isString())
            return fail();
    const bool v5 = o.value(QStringLiteral("schema")).toString() == QStringLiteral("org.ucal.backend-package/5");
    if ((v5 && o.contains(QStringLiteral("channels"))) || !validLocalizedMetadata(o.value("description"))) return fail();
    const bool v4 = v5 || o.value(QStringLiteral("schema")).toString() == QStringLiteral("org.ucal.backend-package/4");
    const bool v3 = v4 || o.value(QStringLiteral("schema")).toString() == QStringLiteral("org.ucal.backend-package/3");
    if (!v3 && o.value(QStringLiteral("schema")).toString() != QStringLiteral("org.ucal.backend-package/2"))
        return fail();
    if ((v3 && (o.contains(QStringLiteral("mode")) || !o.value(QStringLiteral("type")).isString())) ||
        (!v3 && o.contains(QStringLiteral("type")))) return fail();
    p.id = o.value(QStringLiteral("id")).toString();
    p.name = o.value(QStringLiteral("name")).toString();
    p.description = o.value(QStringLiteral("description"));
    p.vendor = o.value(QStringLiteral("vendor")).toString();
    p.version = o.value(QStringLiteral("version")).toString();
    p.mode = o.value(QStringLiteral("mode")).toString();
    p.type = v3 ? o.value(QStringLiteral("type")).toString() :
        (p.mode == QStringLiteral("j2534") ? QStringLiteral("j2534-0404-gmw17753") : p.mode);
    if (v3) {
        if (p.type != QStringLiteral("ucal") && p.type != QStringLiteral("j2534-0404-gmw17753")) return fail();
        p.mode = p.type == QStringLiteral("ucal") ? QStringLiteral("ucal") : QStringLiteral("j2534");
    }
    p.packageOwnsChannels = v3;
    p.runtimeDiscovery = v4 && p.mode == QStringLiteral("j2534");
    if (v4 && o.contains("channels")) return fail();
    if (o.contains("discoveryLibrary")) {
        if (!v4 || p.mode != QStringLiteral("j2534") || !o.value("discoveryLibrary").isString()) return fail();
        const auto discovery = o.value("discoveryLibrary").toString();
        if (!scalar(discovery, 1023)) return fail();
        p.discoveryLibraryPath = resolved(discovery, path);
    }
    if (o.contains("preserveEndpointIdentity") && !o.value("preserveEndpointIdentity").isBool()) return fail();
    p.preserveEndpointIdentity = o.value("preserveEndpointIdentity").toBool();
    if (!parsePackageTool(o.value(QStringLiteral("configurationTool")), path, p.configurationTool, error)) return false;
    if (!identifier(p.id) || !scalar(p.name, 127) || !scalar(p.vendor, 127) || !scalar(p.version, 64) ||
        (p.mode != QStringLiteral("ucal") && p.mode != QStringLiteral("j2534")))
        return fail();
    const auto library = o.value(QStringLiteral("library")).toString();
    if (!scalar(library, 1023))
        return fail();
    p.libraryPath = resolved(library, path);
    p.architecture = runtimeLibraryArchitecture(p.libraryPath);
    if (p.architecture.isEmpty()) {
        error = QStringLiteral("Plugin DLL is missing or has an unsupported PE architecture");
        return false;
    }
    if (!p.discoveryLibraryPath.isEmpty() && runtimeLibraryArchitecture(p.discoveryLibraryPath) != p.architecture) {
        error = QStringLiteral("Discovery DLL is missing or has a different architecture"); return false;
    }
    p.backendFamily = p.mode == QStringLiteral("j2534") ? QStringLiteral("J2534")
                                                        : o.value(QStringLiteral("backendFamily")).toString();
    if (!identifier(p.backendFamily))
        return fail();
    const auto configuration = o.value(QStringLiteral("configuration")).toString();
    if (!scalar(configuration, 1023, true))
        return fail();
    p.configurationPath = resolved(configuration, path);
    p.configurationTemplatePath = p.configurationPath;
    p.capabilities = o.value(QStringLiteral("capabilities")).toString(QStringLiteral("CAN / ISO CAN FD"));
    if (!scalar(p.capabilities, 255))
        return fail();
    p.manifestPath = path;
    p.source = QStringLiteral("backends");
    p.builtIn = false;
    p.enabled = true;
    if (o.contains(QStringLiteral("pins")) && !integer(o.value(QStringLiteral("pins")), p.pins, 65535))
        return fail();
    if (o.contains(QStringLiteral("channels"))) {
        if (p.mode != QStringLiteral("j2534") || !o.value(QStringLiteral("channels")).isArray())
            return fail();
        const auto values = o.value(QStringLiteral("channels")).toArray();
        for (int i = 0; i < values.size(); ++i) {
            if (!values[i].isObject())
                return fail();
            const auto v = values[i].toObject();
            BackendChannelBinding c;
            const QSet<QString> ck{QStringLiteral("id"),           QStringLiteral("adapterId"),
                                   QStringLiteral("name"),         QStringLiteral("selector"),
                                   QStringLiteral("physicalPort"), QStringLiteral("brs"),
                                   QStringLiteral("termination")};
            for (auto x = v.begin(); x != v.end(); ++x)
                if (!ck.contains(x.key()))
                    return fail();
            for (const auto &key : {"id", "name", "selector", "adapterId"})
                if (v.contains(QLatin1String(key)) && !v.value(QLatin1String(key)).isString())
                    return fail();
            c.selector = v.value(QStringLiteral("selector")).toString();
            c.adapterIdentity = v.value(QStringLiteral("adapterId")).toString();
            c.name = v.value(QStringLiteral("name")).toString();
            c.identity = v.value(QStringLiteral("id")).toString();
            if (c.identity.isEmpty())
                c.identity = QStringLiteral("binding-") +
                             QString::fromLatin1(
                                 QCryptographicHash::hash(c.selector.toUtf8(), QCryptographicHash::Sha256)
                                     .toHex()
                                     .left(32));
            c.physicalPort = static_cast<quint32>(i);
            if (v.contains(QStringLiteral("physicalPort")) &&
                !integer(v.value(QStringLiteral("physicalPort")), c.physicalPort, 255))
                return fail();
            if ((v.contains(QStringLiteral("brs")) && !v.value(QStringLiteral("brs")).isBool()) ||
                (v.contains(QStringLiteral("termination")) &&
                 !v.value(QStringLiteral("termination")).isBool()))
                return fail();
            c.brs = v.value(QStringLiteral("brs")).toBool(true);
            c.termination = v.value(QStringLiteral("termination")).toBool(false);
            p.defaultChannels << c;
        }
    }
    if (!channelsValid(p.defaultChannels, &error))
        return false;
    if (o.contains(QStringLiteral("fixedTiming"))) {
        if (p.mode != QStringLiteral("j2534") || !o.value(QStringLiteral("fixedTiming")).isObject())
            return fail();
        const auto timing = o.value(QStringLiteral("fixedTiming")).toObject();
        const QStringList names{QStringLiteral("nominalBitrate"), QStringLiteral("dataBitrate"),
                                QStringLiteral("nominalSample"),  QStringLiteral("dataSample"),
                                QStringLiteral("dllVersion"),     QStringLiteral("sha256")};
        if (timing.size() != names.size())
            return fail();
        for (int i = 0; i < names.size(); ++i) {
            if (i < 4) {
                quint32 n{};
                if (!integer(timing.value(names[i]), n, i < 2 ? 0xffffffffu : 99u) || !n)
                    return fail();
                p.fixedTiming[names[i]] = QString::number(n);
            } else {
                const auto value = timing.value(names[i]).toString();
                if (!scalar(value, i == 4 ? 64 : 64) ||
                    (i == 5 && !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(value).hasMatch()))
                    return fail();
                p.fixedTiming[names[i]] = value;
            }
        }
    }
    return true;
}
QString backendPluginSettingsDirectory() {
    return QDir(QFileInfo(uiLanguagePreferencePath()).absolutePath())
        .filePath(QStringLiteral("backend-plugins"));
}
QString backendPluginCatalogPath() {
    return QDir(backendPluginSettingsDirectory()).filePath(QStringLiteral("catalog.ini"));
}
QVector<BackendChannelBinding> loadBackendPluginChannels(const BackendPackage &p, QString *error) {
    if (error)
        error->clear();
    if (p.runtimeDiscovery) {
        if (!p.discoveryLibraryPath.isEmpty()) return {};
        // A vendor default logical binding is not a discovered physical port.
        BackendChannelBinding binding; binding.identity = QStringLiteral("default");
        binding.name = p.name + QStringLiteral(" · driver default"); return {binding};
    }
    if (p.packageOwnsChannels || !QFileInfo::exists(p.configurationPath))
        return p.defaultChannels;
    // Vendor selectors are opaque. QSettings interprets backslashes and @ syntax,
    // whereas the runtime consumes literal Windows INI values.
    QFile file(p.configurationPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 262144) {
        if (error)
            *error = QStringLiteral("Cannot read saved backend channels");
        return {};
    }
    QTextStream stream(&file);
    stream.setAutoDetectUnicode(true);
    stream.setCodec("UTF-8");
    QMap<QString, QString> values;
    QString section;
    while (!stream.atEnd()) {
        const auto line = stream.readLine();
        const auto trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']')))
            section = trimmed.mid(1, trimmed.size() - 2);
        else if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char(';'))) {
            const auto at = line.indexOf(QLatin1Char('='));
            if (at > 0)
                values.insert(section + QLatin1Char('/') + line.left(at).trimmed(), line.mid(at + 1));
        }
    }
    bool good = false;
    const int count = values.value(QStringLiteral("provider/endpoint_count")).toInt(&good);
    if (!good || count < 0 || count > 64 || values.value(QStringLiteral("provider/id")) != p.id) {
        if (error)
            *error = QStringLiteral("Invalid saved backend channels");
        return {};
    }
    QVector<BackendChannelBinding> result;
    for (int i = 0; i < count; ++i) {
        const auto prefix = QStringLiteral("endpoint.%1/").arg(i);
        BackendChannelBinding c;
        c.identity = values.value(prefix + QStringLiteral("adapter_identity"));
        c.adapterIdentity = values.value(prefix + QStringLiteral("physical_adapter_identity"));
        c.name = values.value(prefix + QStringLiteral("name"));
        c.selector = values.value(prefix + QStringLiteral("selector"));
        c.physicalPort = values.value(prefix + QStringLiteral("physical_port")).toUInt(&good);
        if (!good) {
            if (error)
                *error = QStringLiteral("Invalid saved physical port");
            return {};
        }
        c.brs = values.value(prefix + QStringLiteral("brs"), QStringLiteral("1")) == QStringLiteral("1");
        c.termination =
            values.value(prefix + QStringLiteral("termination"), QStringLiteral("0")) == QStringLiteral("1");
        result << c;
    }
    if (!channelsValid(result, error))
        return {};
    return result;
}
bool saveBackendPluginChannels(const BackendPackage &p, const QVector<BackendChannelBinding> &channels,
                               QString *error) {
    if (error)
        error->clear();
    if (p.mode != QStringLiteral("j2534") || !identifier(p.id) || !scalar(p.libraryPath, 1023) ||
        !QFileInfo(p.libraryPath).isAbsolute() || !channelsValid(channels, error))
        return false;
    QString text =
        QStringLiteral(
            "[provider]\nprofile=04.04-gmw17753-raw-iso-fd\nid=%1\nruntime=%2\nendpoint_count=%3\npins=%4\n")
            .arg(p.id, QDir::toNativeSeparators(p.libraryPath))
            .arg(channels.size())
            .arg(p.pins);
    if (!p.discoveryLibraryPath.isEmpty())
        text += QStringLiteral("discovery_library=%1\n").arg(QDir::toNativeSeparators(p.discoveryLibraryPath));
    const QMap<QString, QString> keys{
        {QStringLiteral("nominalBitrate"), QStringLiteral("fixed_fd_nominal_bitrate")},
        {QStringLiteral("dataBitrate"), QStringLiteral("fixed_fd_data_bitrate")},
        {QStringLiteral("nominalSample"), QStringLiteral("fixed_fd_nominal_sample_point")},
        {QStringLiteral("dataSample"), QStringLiteral("fixed_fd_data_sample_point")},
        {QStringLiteral("dllVersion"), QStringLiteral("fixed_timing_dll_version")},
        {QStringLiteral("sha256"), QStringLiteral("fixed_timing_sha256")}};
    for (auto i = p.fixedTiming.begin(); i != p.fixedTiming.end(); ++i)
        text += keys.value(i.key()) + QLatin1Char('=') + i.value() + QLatin1Char('\n');
    for (int i = 0; i < channels.size(); ++i) {
        const auto &c = channels[i];
        text += QStringLiteral("[endpoint.%1]\nopen_mode=%2\nselector=%3\nadapter_identity=%4\nphysical_"
                               "adapter_identity=%9\nphysical_port="
                               "%5\nname=%6\nmodel=J2534 CAN FD\nserial=\nbrs=%7\ntermination=%8\n")
                    .arg(i)
                    .arg(c.selector.isEmpty() ? QStringLiteral("default") : QStringLiteral("named"),
                         c.selector, c.identity)
                    .arg(c.physicalPort)
                    .arg(c.name)
                    .arg(c.brs ? 1 : 0)
                    .arg(c.termination ? 1 : 0)
                    .arg(c.adapterIdentity);
    }
    return atomicText(p.configurationPath, text, error);
}
QString synchronizeBackendPlugins(QVector<BackendPackage> &packages, const QString &app,
                                  const QString &settingsDirectory) {
    const auto settings = settingsDirectory.isEmpty() ? backendPluginSettingsDirectory() : settingsDirectory;
    QStringList errors;
    QStringList entries;
    int count = 0;
    for (auto &p : packages) {
        if (!p.enabled || p.mode.isEmpty() || !p.manifestAvailable)
            continue;
        p.loadError.clear();
        p.workerPath = runtimeAsset(app, p.architecture, QStringLiteral("UcalBroker.exe"));
        p.adapterPath = p.mode == QStringLiteral("j2534")
                            ? runtimeAsset(app, p.architecture, QStringLiteral("UcalBackendJ2534.dll"))
                            : p.libraryPath;
        if (p.mode == QStringLiteral("j2534")) {
            p.configurationPath = QDir(settings).filePath(p.id + QStringLiteral(".ini"));
            QString error;
            const auto channels = loadBackendPluginChannels(p, &error);
            if (error.isEmpty())
                saveBackendPluginChannels(p, channels, &error);
            if (!error.isEmpty())
                p.loadError = error;
        } else {
            QString error;
            if (!migrateNativeConfiguration(p, settings, &error)) p.loadError = error;
        }
        if (p.adapterPath.isEmpty() || p.workerPath.isEmpty())
            p.loadError = QStringLiteral("Matching plugin adapter or worker is missing");
        if (!p.loadError.isEmpty()) {
            errors << p.name + QStringLiteral(": ") + p.loadError;
            continue;
        }
        entries << QStringLiteral(
                       "[plugin.%1]\nid=%2\nfamily=%3\nlibrary=%4\nconfiguration=%5\nworker=%6\nmode=%7\npreserve_identity=%8\n")
                       .arg(count++)
                       .arg(p.id,
                            p.mode == QStringLiteral("j2534") ? QStringLiteral("j2534") : p.backendFamily,
                            QDir::toNativeSeparators(p.adapterPath),
                            QDir::toNativeSeparators(p.configurationPath),
                            QDir::toNativeSeparators(p.workerPath), p.mode).arg(p.preserveEndpointIdentity ? 1 : 0);
    }
    QString error;
    const auto path = QDir(settings).filePath(QStringLiteral("catalog.ini"));
    if (!atomicText(path,
                    QStringLiteral("[catalog]\nversion=1\ncount=%1\n").arg(count) + entries.join(QString()),
                    &error))
        errors << error;
    return errors.join(QLatin1Char('\n'));
}
} // namespace Ucal::Manager
