#include "Branding.h"
#include <QApplication>
#include <QTextStream>
#include <QDir>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    const auto arguments = application.arguments();
    QString error;
    if (arguments.size() == 4 && arguments[1] == QStringLiteral("--application")) {
        if (!QDir().mkpath(arguments[3])) return 1;
        const auto path = Ucal::Manager::createShortcutIcon(arguments[2], arguments[3], &error);
        if (path.isEmpty() || !QIcon(path).pixmap(256, 256).save(
                QDir(arguments[3]).filePath(QStringLiteral("application.png")))) {
            QTextStream(stderr) << error << '\n'; return 1;
        }
        QTextStream(stdout) << path << '\n';
        return 0;
    }
    if (arguments.size() != 3) return 2;
    if (!Ucal::Manager::writeIconFile(arguments[2], QIcon(arguments[1]), &error)) {
        QTextStream(stderr) << error << '\n'; return 1;
    }
    return 0;
}
