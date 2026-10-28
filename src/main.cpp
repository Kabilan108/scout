#include "logging.h"
#include "mainwindow.h"
#include "service.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>

#include <cstring>
#include <sys/resource.h>

namespace {

// Options that print something and exit don't belong in scout.log.
bool isCommandLineTool(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i)
        for (const char *flag : {"--version", "-v", "--help", "-h", "--help-all"})
            if (std::strcmp(argv[i], flag) == 0)
                return true;
    return false;
}

} // namespace

int main(int argc, char *argv[]) {
    // Deleting deep trees holds one descriptor per level.
    struct rlimit files {};
    // Capped, because launched apps inherit it and select()-based code breaks above 1024 fds.
    if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < 16384) {
        files.rlim_cur = std::min<rlim_t>(files.rlim_max, 16384);
        setrlimit(RLIMIT_NOFILE, &files);
    }
    const bool logToFile = !isCommandLineTool(argc, argv);
    if (logToFile) {
        // Before QApplication, so platform plugin failures reach the log too.
        logging::open(logging::defaultPath());
        logging::installMessageHandler();
        logging::installCrashHandler();
    }
    for (const QString &dir : QStringLiteral(SCOUT_PLUGIN_DIRS).split(u':', Qt::SkipEmptyParts))
        QCoreApplication::addLibraryPath(dir);
    if (!logToFile)
        qputenv("QT_QPA_PLATFORM", "offscreen"); // works without a display
    else if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "wayland;xcb");
    // Our own palette and Fusion style; a desktop platform theme would fight them.
    qunsetenv("QT_QPA_PLATFORMTHEME");
    qunsetenv("QT_STYLE_OVERRIDE");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("scout"));
    QApplication::setApplicationVersion(logging::versionString());
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

    const bool service = parser.isSet(serviceMode);
    if (logToFile)
        logging::info(QStringLiteral("scout %1 starting: mode %2, platform %3, args [%4]")
                          .arg(logging::versionString(), service ? QStringLiteral("service") : QStringLiteral("window"),
                               QGuiApplication::platformName(), QCoreApplication::arguments().mid(1).join(QStringLiteral(", "))));

    Theme::instance().init();
    int status = 0;
    if (service) {
        status = service::run();
    } else {
        const QStringList paths = parser.positionalArguments();
        MainWindow window(paths.value(0, QDir::currentPath()), !parser.isSet(noConfig));
        window.show();
        status = app.exec();
    }
    logging::logExit(status);
    return status;
}
