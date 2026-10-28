#include "startup.h"

#include "mainwindow.h"

#include <QDir>

std::unique_ptr<MainWindow> startup::openWindow(const QStringList &paths, bool loadUserConfig) {
    auto window = std::make_unique<MainWindow>(paths.value(0, QDir::currentPath()), loadUserConfig);
    if (paths.isEmpty())
        window->restoreSession();
    return window;
}
