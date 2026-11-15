#include "PackageTool.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QSet>

namespace Ucal::Manager {
bool parsePackageTool(const QJsonValue &value, const QString &manifest, PackageTool &tool, QString &error) {
    error.clear();
    tool = {};
    if (value.isUndefined())
        return true;
    const auto object = value.toObject();
    const QSet<QString> keys{QStringLiteral("program"), QStringLiteral("arguments"),
                             QStringLiteral("workingDirectory")};
    const auto validText = [](const QString &s) {
        return s.size() <= 4096 && !s.contains(QChar(0)) && !s.contains('\n') && !s.contains('\r');
    };
    const auto fail = [&] {
        error = QStringLiteral("Invalid configurationTool description");
        return false;
    };
    if (!value.isObject() || !object.value(QStringLiteral("program")).isString() ||
        object.value(QStringLiteral("program")).toString().trimmed().isEmpty())
        return fail();
    for (auto i = object.begin(); i != object.end(); ++i)
        if (!keys.contains(i.key()))
            return fail();
    const QDir base(QFileInfo(manifest).absolutePath());
    auto path = [&](const QString &s) { return QDir::cleanPath(base.absoluteFilePath(s)); };
    const auto program = object.value(QStringLiteral("program")).toString();
    if (!validText(program))
        return fail();
    tool.program = path(program);
    if (object.contains(QStringLiteral("arguments"))) {
        if (!object.value(QStringLiteral("arguments")).isArray())
            return fail();
        const auto arguments = object.value(QStringLiteral("arguments")).toArray();
        if (arguments.size() > 128)
            return fail();
        for (const auto &arg : arguments) {
            if (!arg.isString() || !validText(arg.toString()))
                return fail();
            tool.arguments << arg.toString();
        }
    }
    if (object.contains(QStringLiteral("workingDirectory")) &&
        (!object.value(QStringLiteral("workingDirectory")).isString() ||
         !validText(object.value(QStringLiteral("workingDirectory")).toString())))
        return fail();
    const auto directory = object.value(QStringLiteral("workingDirectory")).toString();
    tool.workingDirectory = directory.isEmpty() ? QFileInfo(tool.program).absolutePath() : path(directory);
    return true;
}

bool startPackageTool(const PackageTool &tool, QString *error) {
    if (error)
        error->clear();
    if (!tool.available() || !QFileInfo(tool.program).isFile() || !QFileInfo(tool.workingDirectory).isDir()) {
        if (error)
            *error =
                QStringLiteral("Configuration tool or working directory is missing: %1").arg(tool.program);
        return false;
    }
    QProcess process;
    process.setProgram(tool.program);
    process.setArguments(tool.arguments);
    process.setWorkingDirectory(tool.workingDirectory);
    if (process.startDetached())
        return true;
    if (error)
        *error = QStringLiteral("Could not start %1: %2").arg(tool.program, process.errorString());
    return false;
}
} // namespace Ucal::Manager
