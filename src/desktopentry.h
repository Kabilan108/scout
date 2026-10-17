#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

struct DesktopApp {
    QString id;   // e.g. "org.gnome.Loupe.desktop"
    QString name;
    QString exec;
    QString icon;
    QString file; // path of the .desktop file
    bool terminal = false;
    QStringList mimeTypes;
};

// Freedesktop application lookup: .desktop files plus mimeapps.list.
class DesktopApps {
public:
    static DesktopApps &instance();
    void reload();

    // Apps that can open every one of `mimeTypes`, the user's default first.
    QList<DesktopApp> appsFor(const QStringList &mimeTypes) const;
    QList<DesktopApp> all() const;
    const DesktopApp *find(const QString &id) const;

    // argv lists to run; %f apps are launched once per file.
    static QList<QStringList> commandLines(const DesktopApp &app, const QStringList &paths);
    static QStringList splitExec(const QString &exec);
    static bool launch(const DesktopApp &app, const QStringList &paths, const QString &workingDir, QString *error);

private:
    DesktopApps() = default;
    QStringList candidatesFor(const QString &mime) const;

    // One mimeapps.list, in precedence order (user config first).
    struct Level {
        QHash<QString, QStringList> defaults;
        QHash<QString, QStringList> added;
        QHash<QString, QSet<QString>> removed;
    };

    bool m_loaded = false;
    QHash<QString, DesktopApp> m_apps;
    QList<Level> m_levels;
    QHash<QString, QStringList> m_byMime;
};

QString terminalProgram();
QStringList terminalArgsForDirectory(const QString &terminal, const QString &dir);
// Arguments that come before the command line a terminal should run.
QStringList terminalExecArgs(const QString &terminal);
