#include "launcher.h"

#include <QDesktopServices>
#include <QProcess>

namespace {

launcher::Hook &hook() {
    static launcher::Hook h;
    return h;
}

} // namespace

bool launcher::startDetached(const QString &program, const QStringList &args, const QString &workingDir) {
    if (hook())
        return hook()(program, args, workingDir);
    return QProcess::startDetached(program, args, workingDir);
}

bool launcher::openUrl(const QUrl &url) {
    if (hook())
        return hook()(QStringLiteral("open-url"), {url.toString()}, {});
    return QDesktopServices::openUrl(url);
}

void launcher::setHook(Hook h) { hook() = std::move(h); }
