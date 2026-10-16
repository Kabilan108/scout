#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

struct CommandContext {
    QStringList args;
    int count = 0; // 0 when no count was given
    int countOr(int fallback) const { return count > 0 ? count : fallback; }
};

struct CommandDef {
    QString name;
    QString description;
    QString usage;            // argument hint, e.g. "<path>"; empty for no arguments
    bool requiresArgs = false;
    std::function<void(const CommandContext &)> run;
};

class CommandRegistry {
public:
    void add(CommandDef def);
    void alias(const QString &alias, const QString &target);
    const CommandDef *find(const QString &name) const;
    QList<const CommandDef *> all() const;

    // Splits a command line with shell-like quoting: 'single', "double", \escapes.
    static QStringList split(const QString &line);
    static QString quote(const QString &arg);

private:
    QMap<QString, CommandDef> m_commands;
    QMap<QString, QString> m_aliases;
};

QString expandHome(const QString &path);
