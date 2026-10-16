#include "commands.h"

#include <QDir>

void CommandRegistry::add(CommandDef def) {
    const QString name = def.name;
    m_commands.insert(name, std::move(def));
}

void CommandRegistry::alias(const QString &alias, const QString &target) { m_aliases.insert(alias, target); }

const CommandDef *CommandRegistry::find(const QString &name) const {
    auto it = m_commands.constFind(name);
    if (it != m_commands.constEnd())
        return &it.value();
    auto a = m_aliases.constFind(name);
    if (a != m_aliases.constEnd()) {
        it = m_commands.constFind(a.value());
        if (it != m_commands.constEnd())
            return &it.value();
    }
    return nullptr;
}

QList<const CommandDef *> CommandRegistry::all() const {
    QList<const CommandDef *> out;
    for (const CommandDef &def : m_commands)
        out << &def;
    return out;
}

QStringList CommandRegistry::split(const QString &line) {
    QStringList args;
    QString cur;
    bool hasToken = false;
    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else if (c == u'\\' && quote == u'"' && i + 1 < line.size()) {
                cur += line[++i];
            } else {
                cur += c;
            }
            continue;
        }
        if (c == u'\'' || c == u'"') {
            quote = c;
            hasToken = true;
        } else if (c == u'\\' && i + 1 < line.size()) {
            cur += line[++i];
            hasToken = true;
        } else if (c.isSpace()) {
            if (hasToken)
                args << cur;
            cur.clear();
            hasToken = false;
        } else {
            cur += c;
            hasToken = true;
        }
    }
    if (hasToken)
        args << cur;
    return args;
}

QString CommandRegistry::quote(const QString &arg) {
    if (!arg.isEmpty() && std::none_of(arg.begin(), arg.end(), [](QChar c) {
            return c.isSpace() || c == u'\'' || c == u'"' || c == u'\\';
        }))
        return arg;
    QString escaped = arg;
    escaped.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return u'\'' + escaped + u'\'';
}

QString expandHome(const QString &path) {
    if (path == QLatin1String("~"))
        return QDir::homePath();
    if (path.startsWith(QLatin1String("~/")))
        return QDir::homePath() + path.mid(1);
    return path;
}
