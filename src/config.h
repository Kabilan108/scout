#pragma once

#include <QList>
#include <QString>
#include <QStringList>

class CommandRegistry;
class KeyMap;

struct ConfigLine {
    int number = 0;
    QString text;
    bool run = true; // false when the line has an error that makes running it pointless
};

struct ConfigReport {
    QList<ConfigLine> lines; // every command line, in order
    QStringList errors;      // "line N: ..."
    QStringList notes;       // default bindings the config overrides or removes
    // Plain text for `scout --check-config`; errors come last so they survive a truncated build log.
    QString format(const QString &path) const;
};

namespace config {

// $XDG_CONFIG_HOME/scout/scout.conf
QString defaultPath();

// What `set` accepts. checkOption() returns the problem, or nothing if the value is fine.
QStringList optionNames();
QString checkOption(const QString &name, const QString &value);

// The command line `map <keys> ...` binds: `map gw 'cd ~/work'` and `map gw cd ~/work` agree.
QString mapCommand(const QStringList &mapArgs);

// Checks a config without running it: unknown commands (inside `map` too),
// bad `set` options and values, and bindings that are a strict prefix of
// another or extend one. Only conflicts the config introduces are errors.
ConfigReport analyze(const QString &text, const CommandRegistry &commands, const KeyMap &defaults);

} // namespace config
