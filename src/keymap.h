#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

class QKeyEvent;

struct Binding {
    QStringList keys;  // normalized tokens, e.g. {"g", "g"} or {"<C-d>"}
    QString command;   // a command line, e.g. "cd ~/Downloads"
    QString description;
};

struct Continuation {
    QString key;
    QString label;     // description, command, or "+N more" for a prefix
    bool isPrefix = false;
};

// Vim-style multi-key bindings with an optional count prefix ("5j").
class KeyMap {
public:
    struct Result {
        enum Kind { None, Pending, Command } kind = None;
        QString command;
        int count = 0; // 0 means no count was typed
    };

    void bind(const QString &sequence, const QString &command, const QString &description = {});
    void unbind(const QString &sequence);
    void clear();

    Result feed(const QString &token);
    // Runs an exact binding that was held back because longer bindings share its prefix.
    Result flush();
    void reset();

    bool isPending() const { return !m_pending.isEmpty() || m_count > 0; }
    bool hasPendingKeys() const { return !m_pending.isEmpty(); }
    bool hasAmbiguousExact() const;
    QString pendingDisplay() const;
    QList<Continuation> continuations() const;
    QList<Binding> bindings() const { return m_bindings.values(); }
    QString keysFor(const QString &command) const;

    static QStringList parseSequence(const QString &sequence);
    static QString normalizeToken(const QString &token);
    static QString tokenFor(const QKeyEvent *event);
    static QString display(const QStringList &keys);

private:
    static QString join(const QStringList &keys) { return keys.join(QChar(0x1f)); }
    const Binding *exact(const QStringList &keys) const;
    bool hasLonger(const QStringList &keys) const;

    QMap<QString, Binding> m_bindings;
    QStringList m_pending;
    int m_count = 0;
};
