#pragma once

#include <QString>
#include <QStringList>

class CommandRegistry;
class KeyMap;

// The debug report (`scout --report`, "Copy debug report"): one fenced Markdown
// block for a public issue tracker, so it names no paths under the user's home
// other than scout's own config and log, and no other environment variables.
namespace report {

struct Environment {
    QString platform;
    QString desktop;     // XDG_CURRENT_DESKTOP
    QString sessionType; // XDG_SESSION_TYPE
    QString home;
    QString configPath;
    QString logPath;
};

// The session's values; `platform` is the Qt platform plugin, or what a window would ask for.
Environment current(const QString &platform);

// Reads the config and the log. `commands` and `defaults` check the config as
// `scout --check-config` does.
QString build(const Environment &env, const CommandRegistry &commands, const KeyMap &defaults);

// Every path under `home` (or `~`, or `home` percent-encoded) becomes `~/…`,
// except those in `keep` with no slash after them on the line, which only get
// `home` shortened to `~`. Log lines embed paths with spaces, so a
// path runs through the line's last slash, then to a quote, bracket, comma,
// ": " or the end of the line.
QString redactHome(const QString &text, const QString &home, const QStringList &keep);

} // namespace report
