#include "startup.h"

#include "config.h"
#include "mainwindow.h"

#include <QDir>
#include <QFile>

std::unique_ptr<MainWindow> startup::openWindow(const QStringList &paths, bool loadUserConfig) {
    auto window = std::make_unique<MainWindow>(paths.value(0, QDir::currentPath()), loadUserConfig);
    if (paths.isEmpty())
        window->restoreSession();
    return window;
}

int startup::checkConfig(const QString &path, bool explicitPath, QTextStream &out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (!explicitPath && !f.exists()) {
            out << "no config at " << path << "; scout uses its defaults\n";
            return 0;
        }
        out << "scout.conf: can't read " << path << ": " << f.errorString() << "\n";
        return 1;
    }
    // The command list and default bindings live in a window; it is never shown.
    const MainWindow window(QDir::rootPath(), false);
    const ConfigReport report = config::analyze(QString::fromUtf8(f.readAll()), window.commands(), window.defaultKeys());
    out << report.format(path);
    return report.errors.isEmpty() ? 0 : 1;
}
