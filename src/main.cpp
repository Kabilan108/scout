#include "mainwindow.h"
#include "service.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>

#include <sys/resource.h>

int main(int argc, char *argv[]) {
    // Deleting deep trees holds one descriptor per level.
    struct rlimit files {};
    // Capped, because launched apps inherit it and select()-based code breaks above 1024 fds.
    if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < 16384) {
        files.rlim_cur = std::min<rlim_t>(files.rlim_max, 16384);
        setrlimit(RLIMIT_NOFILE, &files);
    }
    for (const QString &dir : QStringLiteral(SCOUT_PLUGIN_DIRS).split(u':', Qt::SkipEmptyParts))
        QCoreApplication::addLibraryPath(dir);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "wayland;xcb");
    // Our own palette and Fusion style; a desktop platform theme would fight them.
    qunsetenv("QT_QPA_PLATFORMTHEME");
    qunsetenv("QT_STYLE_OVERRIDE");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("scout"));
    QApplication::setApplicationVersion(QStringLiteral(SCOUT_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("scout"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A keyboard-first, ranger-style file manager."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("path"), QStringLiteral("Folder to open, or a file to reveal."));
    QCommandLineOption noConfig(QStringLiteral("no-config"), QStringLiteral("Ignore ~/.config/scout/scout.conf."));
    parser.addOption(noConfig);
    QCommandLineOption serviceMode(QStringLiteral("service"),
                                   QStringLiteral("Run the D-Bus service: org.freedesktop.FileManager1 and the file chooser portal."));
    parser.addOption(serviceMode);
    parser.process(app);

    Theme::instance().init();
    if (parser.isSet(serviceMode))
        return service::run();

    const QStringList paths = parser.positionalArguments();
    MainWindow window(paths.value(0, QDir::currentPath()), !parser.isSet(noConfig));
    window.show();
    return app.exec();
}
