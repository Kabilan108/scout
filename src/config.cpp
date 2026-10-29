#include "config.h"

#include "commands.h"
#include "keymap.h"

#include <QHash>
#include <QSet>
#include <QStandardPaths>

namespace {

const QStringList &boolOptions() {
    static const QStringList names = {QStringLiteral("hidden"), QStringLiteral("dirs-first"), QStringLiteral("reverse"),
                                      QStringLiteral("preview")};
    return names;
}

QString keysText(const QStringList &keys) { return keys.join(QString()); }

QString listSome(QStringList items) {
    std::sort(items.begin(), items.end());
    constexpr int kShown = 6;
    QString out = items.mid(0, kShown).join(QStringLiteral(", "));
    if (items.size() > kShown)
        out += QStringLiteral(" and %1 more").arg(items.size() - kShown);
    return out;
}

// A mapped command may leave out required arguments: the key then opens the prompt for them.
QString checkCommand(const QStringList &words, const CommandRegistry &commands, bool mapped = false) {
    if (words.isEmpty())
        return {};
    const CommandDef *def = commands.find(words.first());
    if (!def)
        return QStringLiteral("unknown command %1").arg(words.first());
    if (def->requiresArgs && words.size() < 2)
        return mapped ? QString() : QStringLiteral("%1 needs %2").arg(def->name, def->usage);
    if (def->name == QLatin1String("set"))
        return config::checkOption(words.value(1), words.mid(2).join(u' '));
    if (def->name == QLatin1String("map")) {
        if (words.size() < 3)
            return QStringLiteral("usage: map <keys> <command>");
        const QStringList keys = KeyMap::parseSequence(words[1]);
        if (keys.isEmpty())
            return QStringLiteral("map needs keys before the command");
        for (const QString &token : keys)
            if (!KeyMap::isValidToken(token))
                return QStringLiteral("map %1: unknown key %2").arg(words[1], token);
        const QStringList inner = CommandRegistry::split(config::mapCommand(words.mid(1)));
        if (inner.isEmpty())
            return QStringLiteral("map %1: no command").arg(words[1]);
        if (const QString problem = checkCommand(inner, commands, true); !problem.isEmpty())
            return QStringLiteral("map %1: %2").arg(words[1], problem);
    }
    return {};
}

} // namespace

QString config::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/scout/scout.conf");
}

QStringList config::optionNames() {
    return boolOptions() + QStringList{QStringLiteral("sort"), QStringLiteral("theme"), QStringLiteral("font"),
                                       QStringLiteral("font-size"), QStringLiteral("editor")};
}

QString config::checkOption(const QString &name, const QString &value) {
    const QString v = value.trimmed().toLower();
    if (name.isEmpty())
        return QStringLiteral("set needs an option: %1").arg(optionNames().join(QStringLiteral(", ")));
    if (boolOptions().contains(name)) {
        static const QSet<QString> ok = {QString(),           QStringLiteral("toggle"), QStringLiteral("true"),
                                         QStringLiteral("false"), QStringLiteral("on"),     QStringLiteral("off"),
                                         QStringLiteral("yes"), QStringLiteral("no"),     QStringLiteral("1"),
                                         QStringLiteral("0")};
        return ok.contains(v) ? QString() : QStringLiteral("%1 takes true, false or toggle, not %2").arg(name, value);
    }
    if (name == QLatin1String("sort")) {
        static const QSet<QString> keys = {QStringLiteral("name"),      QStringLiteral("size"), QStringLiteral("modified"),
                                           QStringLiteral("mtime"),     QStringLiteral("extension"), QStringLiteral("ext")};
        const QStringList words = v.split(u' ', Qt::SkipEmptyParts);
        const bool ok = (words.size() == 1 && (keys.contains(words[0]) || words[0] == QLatin1String("reverse"))) ||
                        (words.size() == 2 && keys.contains(words[0]) && words[1] == QLatin1String("reverse"));
        return ok ? QString() : QStringLiteral("sort takes name, size, modified or extension, then optionally reverse");
    }
    if (name == QLatin1String("theme"))
        return QStringList{QStringLiteral("auto"), QStringLiteral("dark"), QStringLiteral("light")}.contains(v)
                   ? QString()
                   : QStringLiteral("theme takes auto, dark or light");
    if (name == QLatin1String("font"))
        return v.isEmpty() ? QStringLiteral("font needs a family name") : QString();
    if (name == QLatin1String("font-size")) {
        bool ok = false;
        const double size = v.toDouble(&ok);
        return ok && size >= 6 && size <= 32 ? QString() : QStringLiteral("font-size needs a number from 6 to 32");
    }
    if (name == QLatin1String("editor"))
        return {};
    return QStringLiteral("unknown option: %1").arg(name);
}

QString config::mapCommand(const QStringList &mapArgs) {
    if (mapArgs.size() == 2)
        return mapArgs[1];
    QStringList rest;
    for (const QString &a : mapArgs.mid(1))
        rest << CommandRegistry::quote(a);
    return rest.join(u' ');
}

ConfigReport config::analyze(const QString &text, const CommandRegistry &commands, const KeyMap &defaults) {
    ConfigReport report;
    QList<std::pair<int, QString>> errors;
    KeyMap keys = defaults;
    QHash<QString, int> mappedAt; // keys text -> line of the `map` that bound it
    const QStringList raw = text.split(u'\n');
    for (int i = 0; i < raw.size(); ++i) {
        const QString line = raw[i].trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        ConfigLine entry{i + 1, line, true};
        const QStringList words = CommandRegistry::split(line);
        if (const QString problem = checkCommand(words, commands); !problem.isEmpty()) {
            errors.append({entry.number, problem});
            entry.run = false;
            report.lines << entry;
            continue;
        }
        const QString name = words.isEmpty() ? QString() : commands.find(words.first())->name;
        if (name == QLatin1String("map")) {
            const QString seq = keysText(KeyMap::parseSequence(words[1]));
            const QString command = mapCommand(words.mid(1));
            if (const Binding *d = defaults.binding(words[1]); d && d->command != command)
                report.notes << QStringLiteral("line %1: map %2 overrides the default %2 (%3)").arg(entry.number).arg(seq, d->command);
            keys.bind(words[1], command, command, true);
            mappedAt.insert(seq, entry.number);
        } else if (name == QLatin1String("unmap")) {
            const QString seq = keysText(KeyMap::parseSequence(words[1]));
            if (!keys.binding(words[1])) {
                report.notes << QStringLiteral("line %1: unmap %2 does nothing; %2 isn't bound").arg(entry.number).arg(seq);
            } else {
                if (!keys.isFromConfig(words[1]))
                    report.notes << QStringLiteral("line %1: unmap %2 removes the default %2 (%3)")
                                        .arg(entry.number)
                                        .arg(seq, keys.binding(words[1])->command);
                keys.unbind(words[1]);
                mappedAt.remove(seq);
            }
        }
        report.lines << entry;
    }

    // Each conflicting pair is reported once, at the later of the two lines.
    const QList<Binding> all = keys.bindings();
    for (const Binding &b : all) {
        if (!b.fromConfig)
            continue;
        const int line = mappedAt.value(keysText(b.keys));
        QStringList longer, shorter;
        for (const Binding &o : all) {
            if (o.keys == b.keys || (o.fromConfig && mappedAt.value(keysText(o.keys)) > line))
                continue;
            if (o.keys.size() > b.keys.size() && o.keys.mid(0, b.keys.size()) == b.keys)
                longer << keysText(o.keys);
            else if (o.keys.size() < b.keys.size() && b.keys.mid(0, o.keys.size()) == o.keys)
                shorter << keysText(o.keys);
        }
        const QString seq = keysText(b.keys);
        if (!longer.isEmpty())
            errors.append({line, QStringLiteral("map %1 is a prefix of other bindings (%2), so it only runs after a pause")
                                     .arg(seq, listSome(longer))});
        for (const QString &s : std::as_const(shorter))
            errors.append({line, QStringLiteral("map %1 starts with %2, which is bound on its own, so %2 only runs after a pause")
                                     .arg(seq, s)});
    }
    std::stable_sort(errors.begin(), errors.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[line, text] : std::as_const(errors))
        report.errors << QStringLiteral("line %1: %2").arg(line).arg(text);
    return report;
}

QString ConfigReport::format(const QString &path) const {
    QString out = QStringLiteral("scout.conf: %1\n").arg(path);
    if (!notes.isEmpty()) {
        out += QStringLiteral("info:\n");
        for (const QString &n : notes)
            out += QStringLiteral("  %1\n").arg(n);
    }
    if (!errors.isEmpty()) {
        out += QStringLiteral("errors:\n");
        for (const QString &e : errors)
            out += QStringLiteral("  %1\n").arg(e);
    }
    out += errors.isEmpty() ? QStringLiteral("no errors\n")
                            : QStringLiteral("%1 error%2\n").arg(errors.size()).arg(errors.size() == 1 ? "" : "s");
    return out;
}
