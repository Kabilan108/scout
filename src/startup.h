#pragma once

#include <QStringList>
#include <QTextStream>

#include <memory>

class MainWindow;

namespace startup {

// The window `scout [path]` opens: the path if one was given, otherwise the
// tabs of the window closed last, or the current folder the first time.
// Service, chooser and new-window windows never restore tabs.
std::unique_ptr<MainWindow> openWindow(const QStringList &paths, bool loadUserConfig);

// `scout --check-config [FILE]`: prints the report to `out` and returns the
// exit status, 1 if the config has errors. Needs a QApplication but shows nothing.
int checkConfig(const QString &path, bool explicitPath, QTextStream &out);

} // namespace startup
