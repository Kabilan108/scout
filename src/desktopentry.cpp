#include "desktopentry.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

namespace {

QStringList dataDirs() {
    QStringList dirs;
    QString home = qEnvironmentVariable("XDG_DATA_HOME");
    if (home.isEmpty())
        home = QDir::homePath() + QStringLiteral("/.local/share");
    dirs << home;
    QString rest = qEnvironmentVariable("XDG_DATA_DIRS");
    if (rest.isEmpty())
        rest = QStringLiteral("/usr/local/share:/usr/share");
    dirs << rest.split(u':', Qt::SkipEmptyParts);
    dirs.removeDuplicates();
    return dirs;
}

QStringList configDirs() {
    QStringList dirs;
    QString home = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (home.isEmpty())
        home = QDir::homePath() + QStringLiteral("/.config");
    dirs << home;
    QString rest = qEnvironmentVariable("XDG_CONFIG_DIRS");
    if (rest.isEmpty())
        rest = QStringLiteral("/etc/xdg");
    dirs << rest.split(u':', Qt::SkipEmptyParts);
    return dirs;
}

using Groups = QHash<QString, QHash<QString, QString>>;

Groups parseIni(const QString &path) {
    Groups groups;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return groups;
    QTextStream in(&f);
    QString group;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        if (line.startsWith(u'[') && line.endsWith(u']')) {
            group = line.mid(1, line.size() - 2);
            continue;
        }
        const int eq = int(line.indexOf(u'='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        if (!groups[group].contains(key))
            groups[group].insert(key, line.mid(eq + 1).trimmed());
    }
    return groups;
}

QStringList splitList(const QString &value) { return value.split(u';', Qt::SkipEmptyParts); }

QString unescapeValue(const QString &v) {
    QString out;
    for (int i = 0; i < v.size(); ++i) {
        if (v[i] == u'\\' && i + 1 < v.size()) {
            const QChar n = v[++i];
            if (n == u's')
                out += u' ';
            else if (n == u'n')
                out += u'\n';
            else if (n == u't')
                out += u'\t';
            else
                out += n;
        } else {
            out += v[i];
        }
    }
    return out;
}

} // namespace

DesktopApps &DesktopApps::instance() {
    static DesktopApps apps;
    if (!apps.m_loaded)
        apps.reload();
    return apps;
}

void DesktopApps::reload() {
    m_loaded = true;
    m_apps.clear();
    m_byMime.clear();
    m_defaults.clear();
    m_added.clear();
    m_removed.clear();

    for (const QString &base : dataDirs()) {
        const QString appsDir = base + QStringLiteral("/applications");
        QDirIterator it(appsDir, {QStringLiteral("*.desktop")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            QString id = QDir(appsDir).relativeFilePath(file);
            id.replace(u'/', u'-');
            if (m_apps.contains(id))
                continue;
            const auto groups = parseIni(file);
            const auto entry = groups.value(QStringLiteral("Desktop Entry"));
            DesktopApp app;
            app.id = id;
            app.file = file;
            app.name = entry.value(QStringLiteral("Name"));
            app.exec = entry.value(QStringLiteral("Exec"));
            app.icon = entry.value(QStringLiteral("Icon"));
            app.terminal = entry.value(QStringLiteral("Terminal")) == QLatin1String("true");
            app.mimeTypes = splitList(entry.value(QStringLiteral("MimeType")));
            const bool hidden = entry.value(QStringLiteral("Hidden")) == QLatin1String("true");
            const bool application = entry.value(QStringLiteral("Type"), QStringLiteral("Application")) ==
                                     QLatin1String("Application");
            // Register the id even when unusable so lower-priority dirs can't resurrect it.
            if (hidden || !application || app.exec.isEmpty()) {
                app.exec.clear();
                m_apps.insert(id, app);
                continue;
            }
            m_apps.insert(id, app);
            for (const QString &mime : app.mimeTypes)
                m_byMime[mime] << id;
        }
    }

    QStringList mimeappsFiles;
    for (const QString &dir : configDirs())
        mimeappsFiles << dir + QStringLiteral("/mimeapps.list");
    for (const QString &dir : dataDirs())
        mimeappsFiles << dir + QStringLiteral("/applications/mimeapps.list");
    for (const QString &file : mimeappsFiles) {
        const auto groups = parseIni(file);
        const auto defaults = groups.value(QStringLiteral("Default Applications"));
        for (auto it = defaults.begin(); it != defaults.end(); ++it)
            m_defaults[it.key()] << splitList(it.value());
        const auto added = groups.value(QStringLiteral("Added Associations"));
        for (auto it = added.begin(); it != added.end(); ++it)
            m_added[it.key()] << splitList(it.value());
        const auto removed = groups.value(QStringLiteral("Removed Associations"));
        for (auto it = removed.begin(); it != removed.end(); ++it)
            for (const QString &id : splitList(it.value()))
                m_removed[it.key()].insert(id);
    }
}

const DesktopApp *DesktopApps::find(const QString &id) const {
    auto it = m_apps.constFind(id);
    if (it == m_apps.constEnd() || it->exec.isEmpty())
        return nullptr;
    return &it.value();
}

QStringList DesktopApps::candidatesFor(const QString &mime) const {
    QMimeDatabase db;
    QStringList chain{mime};
    const QMimeType type = db.mimeTypeForName(mime);
    if (type.isValid()) {
        chain << type.aliases();
        chain << type.allAncestors();
    }
    QStringList ids;
    auto push = [&](const QString &m, const QStringList &list) {
        for (const QString &id : list)
            if (!m_removed.value(m).contains(id) && find(id) && !ids.contains(id))
                ids << id;
    };
    for (const QString &m : chain) {
        push(m, m_defaults.value(m));
        push(m, m_added.value(m));
        push(m, m_byMime.value(m));
    }
    return ids;
}

QList<DesktopApp> DesktopApps::appsFor(const QStringList &mimeTypes) const {
    if (mimeTypes.isEmpty())
        return {};
    QStringList ids = candidatesFor(mimeTypes.first());
    for (int i = 1; i < mimeTypes.size(); ++i) {
        const QStringList other = candidatesFor(mimeTypes[i]);
        ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const QString &id) { return !other.contains(id); }),
                  ids.end());
    }
    QList<DesktopApp> apps;
    for (const QString &id : ids)
        apps << *find(id);
    return apps;
}

QList<DesktopApp> DesktopApps::all() const {
    QList<DesktopApp> apps;
    for (const DesktopApp &a : m_apps)
        if (!a.exec.isEmpty())
            apps << a;
    std::sort(apps.begin(), apps.end(),
              [](const DesktopApp &a, const DesktopApp &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    return apps;
}

QStringList DesktopApps::splitExec(const QString &rawExec) {
    const QString exec = unescapeValue(rawExec);
    QStringList args;
    QString cur;
    bool inQuotes = false;
    bool hasToken = false;
    for (int i = 0; i < exec.size(); ++i) {
        const QChar c = exec[i];
        if (inQuotes) {
            if (c == u'\\' && i + 1 < exec.size()) {
                cur += exec[++i];
            } else if (c == u'"') {
                inQuotes = false;
            } else {
                cur += c;
            }
        } else if (c == u'"') {
            inQuotes = true;
            hasToken = true;
        } else if (c.isSpace()) {
            if (hasToken || !cur.isEmpty())
                args << cur;
            cur.clear();
            hasToken = false;
        } else {
            cur += c;
        }
    }
    if (hasToken || !cur.isEmpty())
        args << cur;
    return args;
}

QList<QStringList> DesktopApps::commandLines(const DesktopApp &app, const QStringList &paths) {
    const QStringList tokens = splitExec(app.exec);
    bool single = false;
    bool any = false;
    for (const QString &t : tokens) {
        if (t.contains(QLatin1String("%f")) || t.contains(QLatin1String("%u")))
            single = any = true;
        if (t.contains(QLatin1String("%F")) || t.contains(QLatin1String("%U")))
            any = true;
    }

    auto expand = [&](const QStringList &files) {
        QStringList argv;
        for (const QString &t : tokens) {
            if (t == QLatin1String("%F")) {
                argv << files;
                continue;
            }
            if (t == QLatin1String("%U")) {
                for (const QString &f : files)
                    argv << QUrl::fromLocalFile(f).toString();
                continue;
            }
            if (t == QLatin1String("%i")) {
                if (!app.icon.isEmpty())
                    argv << QStringLiteral("--icon") << app.icon;
                continue;
            }
            QString out;
            for (int i = 0; i < t.size(); ++i) {
                if (t[i] != u'%' || i + 1 >= t.size()) {
                    out += t[i];
                    continue;
                }
                const QChar code = t[++i];
                if (code == u'f')
                    out += files.value(0);
                else if (code == u'u')
                    out += files.isEmpty() ? QString() : QUrl::fromLocalFile(files.first()).toString();
                else if (code == u'c')
                    out += app.name;
                else if (code == u'k')
                    out += app.file;
                else if (code == u'%')
                    out += u'%';
            }
            if (!out.isEmpty())
                argv << out;
        }
        if (!any)
            argv << files;
        if (app.terminal) {
            const QString term = terminalProgram();
            if (!term.isEmpty())
                argv = QStringList{term, QStringLiteral("-e")} + argv;
        }
        return argv;
    };

    QList<QStringList> lines;
    if (single && paths.size() > 1) {
        for (const QString &p : paths)
            lines << expand({p});
    } else {
        lines << expand(paths);
    }
    return lines;
}

bool DesktopApps::launch(const DesktopApp &app, const QStringList &paths, const QString &workingDir, QString *error) {
    for (const QStringList &argv : commandLines(app, paths)) {
        if (argv.isEmpty())
            continue;
        QStringList args = argv.mid(1);
        if (!QProcess::startDetached(argv.first(), args, workingDir)) {
            if (error)
                *error = QStringLiteral("could not start %1").arg(argv.first());
            return false;
        }
    }
    return true;
}

QString terminalProgram() {
    const QString fromEnv = qEnvironmentVariable("TERMINAL");
    if (!fromEnv.isEmpty() && !QStandardPaths::findExecutable(fromEnv).isEmpty())
        return fromEnv;
    for (const char *t : {"ghostty", "kitty", "foot", "alacritty", "wezterm", "konsole", "gnome-terminal", "xterm"}) {
        const QString path = QStandardPaths::findExecutable(QLatin1String(t));
        if (!path.isEmpty())
            return QLatin1String(t);
    }
    return {};
}

QStringList terminalArgsForDirectory(const QString &terminal, const QString &dir) {
    const QString name = QFileInfo(terminal).fileName();
    if (name == QLatin1String("ghostty") || name == QLatin1String("foot") || name == QLatin1String("alacritty"))
        return {QStringLiteral("--working-directory=") + dir};
    if (name == QLatin1String("kitty"))
        return {QStringLiteral("--directory"), dir};
    if (name == QLatin1String("wezterm"))
        return {QStringLiteral("start"), QStringLiteral("--cwd"), dir};
    if (name == QLatin1String("konsole"))
        return {QStringLiteral("--workdir"), dir};
    if (name == QLatin1String("gnome-terminal"))
        return {QStringLiteral("--working-directory=") + dir};
    return {};
}
