#pragma once

#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <functional>

class Task : public QObject {
    Q_OBJECT
public:
    // Restore moves each source to an exact destination path; undo uses it.
    enum Kind { Copy, Move, Link, Trash, Delete, Restore };
    enum State { Running, Done, Failed, Cancelled };

    struct Progress {
        State state = Running;
        qint64 totalBytes = 0;
        qint64 doneBytes = 0;
        int totalItems = 0;
        int doneItems = 0;
        QString current;
        QStringList errors;
        QStringList created; // destination paths of copied/moved/linked/restored items
        QList<QPair<QString, QString>> moved; // (from, to) for moves, trashing and restores
    };

    Task(Kind kind, QStringList sources, QString targetDir, QObject *parent = nullptr);
    // For Restore: one destination per source.
    void setDestinations(QStringList destinations) { m_destinations = std::move(destinations); }

    Kind kind() const { return m_kind; }
    QStringList sources() const { return m_sources; }
    QString targetDir() const { return m_targetDir; }
    QString title() const;
    QString verb() const;
    Progress progress() const;
    double fraction() const;
    bool isFinished() const { return progress().state != Running; }
    void cancel() { m_cancel = true; }

    // Runs on a worker thread.
    void run();

signals:
    void finished();

private:
    void update(const std::function<void(Progress &)> &fn);
    bool cancelled() const { return m_cancel.load(); }

    Kind m_kind;
    QStringList m_sources;
    QString m_targetDir;
    QStringList m_destinations;
    mutable QMutex m_mutex;
    Progress m_progress;
    std::atomic_bool m_cancel{false};
};

class TaskManager : public QObject {
    Q_OBJECT
public:
    explicit TaskManager(QObject *parent = nullptr);
    ~TaskManager() override;

    Task *start(Task::Kind kind, const QStringList &sources, const QString &targetDir = {});
    // Moves each pair's first path to exactly its second path, never replacing anything.
    Task *startRestore(const QList<QPair<QString, QString>> &moves);
    QList<Task *> tasks() const { return m_tasks; }
    int activeCount() const;
    void clearFinished();
    // Blocks until every running task finishes. Used by tests and on quit.
    bool waitForAll(int timeoutMs);

signals:
    void tasksChanged();
    void taskFinished(Task *task);

private:
    Task *launch(Task *task);

    QList<Task *> m_tasks;
    QSet<Task *> m_delivered;
    QTimer m_poll;
};

// First free "name", "name (2)", ... in `dir`; empty if none is free.
QString uniqueDestination(const QString &dir, const QString &name);
// rename() that fails with EEXIST instead of replacing; returns 0 or an errno.
int renameNoReplace(const QString &from, const QString &to);
// The fallback for filesystems without RENAME_NOREPLACE.
int renameByReservation(const QString &from, const QString &to);
// Renames `from` into `dir` under the first free variant of `name`.
QString moveToUnique(const QString &from, const QString &dir, const QString &name, int *errorNumber);
// True if `dir` is `folder` or below it, following symlinks in `dir`.
bool isSameOrInside(const QString &dir, const QString &folder);
// Where a file in a freedesktop trash ("<trash>/files/<name>") came from; empty if unknown.
QString trashOriginalPath(const QString &trashedPath);
// The .trashinfo file that belongs to a trashed path; empty if it isn't one.
QString trashInfoPath(const QString &trashedPath);
// Renames every (from, to) pair, staging through temporary names so swaps and
// cycles work. Never replaces a path that isn't itself being renamed away.
// Returns the pairs that were applied; problems go to `errors`.
QList<QPair<QString, QString>> renameAll(const QList<QPair<QString, QString>> &renames, QStringList *errors);
