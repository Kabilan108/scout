#include "startup.h"

#include "config.h"
#include "mainwindow.h"
#include "report.h"

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

QByteArray startup::windowPlatform() {
    if (!qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        return qgetenv("QT_QPA_PLATFORM");
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        return "wayland;xcb";
    return {};
}

int startup::printReport(const QByteArray &windowPlatform, QTextStream &out) {
    const QString requested = windowPlatform.isEmpty() ? QStringLiteral("Qt's default") : QString::fromLocal8Bit(windowPlatform);
    const MainWindow window(QDir::rootPath(), false);
    out << report::build(report::current(QStringLiteral("%1 requested (--report runs without a display)").arg(requested)),
                         window.commands(), window.defaultKeys());
    return 0;
}
