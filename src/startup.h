#pragma once

#include <QStringList>

#include <memory>

class MainWindow;

namespace startup {

// The window `scout [path]` opens: the path if one was given, otherwise the
// tabs of the window closed last, or the current folder the first time.
// Service, chooser and new-window windows never restore tabs.
std::unique_ptr<MainWindow> openWindow(const QStringList &paths, bool loadUserConfig);

} // namespace startup
