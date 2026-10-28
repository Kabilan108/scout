#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

// Every program scout starts for the user (apps, terminals, editors, new
// windows) and every URL it hands to the desktop goes through here, so tests
// can see the launches without starting anything.
namespace launcher {

bool startDetached(const QString &program, const QStringList &args, const QString &workingDir);
bool openUrl(const QUrl &url);

// Replaces both functions. For openUrl the program is "open-url" and the URL is the only argument.
using Hook = std::function<bool(const QString &program, const QStringList &args, const QString &workingDir)>;
void setHook(Hook hook);

} // namespace launcher
