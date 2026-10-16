#pragma once

#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <functional>

class Task : public QObject {
    Q_OBJECT
public:
    enum Kind { Copy, Move, Link, Trash, Delete };
    enum State { Running, Done, Failed, Cancelled };

    struct Progress {
        State state = Running;
        qint64 totalBytes = 0;
        qint64 doneBytes = 0;
        int totalItems = 0;
        int doneItems = 0;
        QString current;
        QStringList errors;
        QStringList created; // destination paths of copied/moved/linked items
    };

    Task(Kind kind, QStringList sources, QString targetDir, QObject *parent = nullptr);

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
    QList<Task *> tasks() const { return m_tasks; }
    int activeCount() const;
    void clearFinished();
    // Blocks until every running task finishes. Used by tests and on quit.
    bool waitForAll(int timeoutMs);

signals:
    void tasksChanged();
    void taskFinished(Task *task);

private:
    QList<Task *> m_tasks;
    QSet<Task *> m_delivered;
    QTimer m_poll;
};

QString uniqueDestination(const QString &dir, const QString &name);
